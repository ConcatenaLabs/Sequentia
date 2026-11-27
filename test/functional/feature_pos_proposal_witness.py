#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A proposal's body that does not match its signed header never costs the leader its round.

Three nodes each hold one of the three committee keys (quorum two). A transaction spends a P2WSH output whose
script drops one witness item, so its witness is anyone's to change: variants with one txid and different
witnesses.

mempool_variant: between two blocks, the next leader (the lowest VRF draw) gets one variant and both other
  members the other. A member rebuilding the compact proposal from its own mempool gets a block whose witness
  commitment does not match; it must fetch the leader's full proposal and sign it. Before the fix both members
  excluded the leader, so a member's block, with the members' variant, took the height.
forged_witness, forged_transactions: one member is cut off from the others and hears proposals only from a
  relay, which takes the leader's full proposal and sends it first with one witness item changed (or with
  another transaction in its place, which breaks the merkle root), then as it is. The forged body is the
  relay's fault: the member penalises the relay, excludes nobody, and signs the leader's block when the genuine
  body arrives, and relays nothing of the forged body. Before the fix the forged body was taken as the
  leader's, the member excluded the leader, and the genuine body was ignored as already seen.
second_lock: the same, with the member's check of a body against its header switched off
  (-posgossipnobodycheck): the round itself drops the altered body as one its header does not bind, fetches
  the body again from the relay, and signs the leader's block; it never excludes the leader for it.
first_announcer_lies: a cut-off member holding the other witness hears the compact proposal from two relays;
  it rebuilds a body that does not match, asks the first relay, which answers with an altered body, and then
  asks the second at once, which answers with the genuine one: the member signs the leader's block within
  seconds, not after the 10 s a request to the same peer waits.
flood_keeps_the_fetch_book: a cut-off member holding the other witness hears the compact proposal from a
  silent announcer and an honest one, and then a third peer floods it with 1,001 compact proposals with
  distinct made-up parents. The flood removes nothing the other two announced: when the silent announcer's
  second is up, the member asks the honest one and signs. Before, a clear-all at 1,000 entries left it nobody
  to ask.
solution_appended_small, solution_appended_large: the leader's full proposal with bytes appended to its staging
  solution (outside the block hash, inside its weight; the large one past the weight limit) is the sender's
  fault: refused and penalised, nobody excluded, the genuine one signed.
padded_r, padded_s, signature_73_bytes, high_s: the leader's own signature re-encoded, still one push that
  verifies under the lax DER parsing of signature checks: 420,000 zero bytes in front of R (or of S) with
  indefinite lengths, a 73-byte encoding, the high S. Each is the sender's fault: refused before anything is
  marked, the sender penalised, the genuine one signed.
padded_signature_reaches_the_round: the padded R with the member's checks of a body switched off
  (-posgossipnobodycheck): the round judges the block with a placeholder of the largest certificate in the
  staging solution's place, so the relay's bytes decide nothing: nobody is excluded and the leader's block is
  signed.
heavy_block_signed: an honest block built to the weight limit is still signed.
bad_signature_keeps_the_fetch_open: a body matching the header but with one byte of the staging signature
  changed is refused, and the member, which lacks the proposal's transaction, still fetches the body from an
  honest announcer and signs.
stale_parent: a member about to sign the leader's proposal finds its parent is no longer the tip (forced with
  -posgossipstaleonce): nothing was judged, so it drops the proposal as stale and excludes nobody, and with the
  third node cut off the height is certified on a block the same leader proposed, signed by that member.
"""

import hashlib
import os
import re
import threading
import time

from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutWitness
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.script import CScript, OP_0, OP_DROP, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

SECP256K1_ORDER = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
SPACING = 8   # -posblockspacing: the quiet seconds between blocks in which a case is set up
POS_MESSAGES = ["posproposal", "poscmpctprop", "getposprop", "posshare", "poscert", "getposcert"]


def raw_message(name):
    """A PoS gossip message carried as its raw payload."""
    class Raw:
        msgtype = name.encode()

        def __init__(self, payload=b""):
            self.payload = payload

        def deserialize(self, f):
            self.payload = f.read()

        def serialize(self):
            return self.payload

        def __repr__(self):
            return "msg_%s(%d bytes)" % (name, len(self.payload))
    return Raw


RAW = {name: raw_message(name) for name in POS_MESSAGES}
for _name, _cls in RAW.items():
    MESSAGEMAP[_name.encode()] = _cls


class PosRelay(P2PInterface):
    """A peer that takes PoS gossip and keeps every payload of each kind; asked for a proposal's body
    (getposprop), it answers with `answer` when one is set."""
    def __init__(self):
        super().__init__()
        self.got = {}
        self.answer = None


def _keep(name):
    def on(self, message):
        self.got.setdefault(name, []).append(message.payload)
        if name == "getposprop" and self.answer is not None:
            self.send_message(RAW["posproposal"](self.answer))
    return on


for _name in POS_MESSAGES:
    setattr(PosRelay, "on_" + _name, _keep(_name))


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosProposalWitnessTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-posblockspacing=%d" % SPACING, "-posblockspacingheight=1",
            "-con_max_block_sig_size=4000", "-signblockscript=51",
            "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1", "-con_blocksubsidy=0",
            "-anyonecanspendaremine=1", "-acceptnonstdtxn=1", "-validatepegin=0", "-debug=net", "-debug=validation",
            "-con_maxblockweight=400000",   # the testnet's weight, so that appended solution bytes can pass it
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]] for i in range(3)]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def wait_height(self, h, timeout=120):
        self.wait_until(lambda: all(n.getblockcount() >= h for n in self.nodes), timeout=timeout)

    def log_lines(self, i):
        return open("%s/%s/debug.log" % (self.nodes[i].datadir, self.chain), encoding="utf8").read().splitlines()

    def leader_of_next(self):
        """The node whose key drew the lowest VRF output for the next block (the committee backs the lowest,
        compared as a uint256 in byte order)."""
        draws = []
        for i, n in enumerate(self.nodes):
            s = n.getposslot()
            draws.append((bytes.fromhex(s["keys"][0]["vrf"])[::-1], i, s["height"]))
        assert_equal(len({h for _, _, h in draws}), 1)
        return min(draws)[1], draws[0][2]

    def fund(self):
        """A confirmed coin paying P2WSH(self.script): (txid, vout, value)."""
        spk = CScript([OP_0, hashlib.sha256(bytes(self.script)).digest()])
        gtx, gn, gval = self.genesis_coin
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(gtx, 16), gn))]
        amount, fee = 10 * 100_000_000, 100_000
        tx.vout = [CTxOut(amount, spk), CTxOut(gval - amount - fee, CScript([OP_TRUE])), CTxOut(fee)]
        tx.rehash()
        self.nodes[0].sendrawtransaction(tx.serialize().hex())
        self.wait_until(lambda: all(n.gettxout(tx.hash, 0, False) is not None for n in self.nodes), timeout=120)
        self.genesis_coin = (tx.hash, 1, gval - amount - fee)
        return tx.hash, 0, amount

    def variants(self, coin_txid, vout, value):
        """Two spends of a P2WSH(self.script) coin: one txid, two witnesses."""
        fee = 10_000
        out = []
        for item in (b"\x01", b"\x02"):
            tx = CTransaction()
            tx.nVersion = 2
            tx.vin = [CTxIn(COutPoint(int(coin_txid, 16), vout), nSequence=0xfffffffd)]
            tx.vout = [CTxOut(value - fee, CScript([OP_TRUE])), CTxOut(fee)]
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = [item, bytes(self.script)]
            tx.wit.vtxoutwit = [CTxOutWitness() for _ in tx.vout]
            tx.rehash()
            out.append(tx)
        assert_equal(out[0].hash, out[1].hash)
        assert out[0].getwtxid() != out[1].getwtxid()
        return out

    def run_test(self):
        n0 = self.nodes[0]
        self.wait_height(2)
        g = n0.getblock(n0.getblockhash(0), 2)
        self.genesis_coin = [(t["txid"], o["n"], int(o["value"] * 100_000_000)) for t in g["tx"] for o in t["vout"]
                             if o["scriptPubKey"]["hex"] == "51" and o.get("value", 0) > 0 and
                             n0.gettxout(t["txid"], o["n"])][0]
        self.script = CScript([OP_DROP, OP_TRUE])
        only = os.environ.get("POS_CASES")
        for case in only.split(",") if only else ["mempool_variant", "forged_witness", "forged_transactions",
                                                  "second_lock", "first_announcer_lies", "stale_parent",
                                                  "flood_keeps_the_fetch_book", "solution_appended_small",
                                                  "solution_appended_large", "padded_r", "padded_s", "signature_73_bytes",
                                                  "high_s", "padded_signature_reaches_the_round",
                                                  "heavy_block_signed", "bad_signature_keeps_the_fetch_open"]:
            self.log.info(case)
            getattr(self, case)()

    def mempool_variant(self):
        attempts = []
        for attempt in range(3):
            v1, v2 = self.variants(*self.fund())
            h0 = self.nodes[0].getblockcount()
            self.wait_height(h0 + 1)          # just after a block: SPACING seconds before the next one
            leader, height = self.leader_of_next()
            members = [i for i in range(3) if i != leader]
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())
            for i in members:
                self.nodes[i].sendrawtransaction(v2.serialize().hex())
            placed = (all(n.getblockcount() == height - 1 for n in self.nodes) and
                      self.nodes[leader].getmempoolentry(v1.hash)["wtxid"] == v1.getwtxid() and
                      all(self.nodes[i].getmempoolentry(v1.hash)["wtxid"] == v2.getwtxid() for i in members))
            logs = [len(self.log_lines(i)) for i in range(3)]
            self.wait_height(height)
            blk = self.nodes[0].getblock(self.nodes[0].getblockhash(height), 2)
            held = [t for t in blk["tx"] if t["txid"] == v1.hash]
            excluded = [line for i in range(3) for line in self.log_lines(i)[logs[i]:] if "excluding its leader" in line]
            attempts.append((attempt, leader, height, placed, held[0]["hash"] if held else None, len(excluded)))
            self.log.info("    attempt %d: leader node %d at height %d, variants placed %s; block holds %s; %d exclusion(s)",
                          attempt, leader, height, placed, held[0]["hash"] if held else "neither", len(excluded))
            if not placed or not held:
                self.wait_until(lambda: all(v1.hash not in n.getrawmempool() for n in self.nodes), timeout=120)
                continue
            assert_equal(held[0]["hash"], v1.getwtxid())  # the leader's variant: the leader's block took the height
            assert_equal(excluded, [])
            return
        raise AssertionError("the case was not reached: %s" % attempts)

    def forged_witness(self):
        self.forged_body(lambda body, v1, v2, other: body.replace(v1.serialize(), v2.serialize()))

    def forged_transactions(self):
        self.forged_body(lambda body, v1, v2, other: body.replace(v1.serialize(), other.serialize()))

    def second_lock(self):
        self.forged_body(lambda body, v1, v2, other: body.replace(v1.serialize(), v2.serialize()), second_lock=True)

    def forged_body(self, forge, second_lock=False):
        """A relay sends a cut-off member the leader's full proposal forged by `forge`, then the genuine one."""
        attempts = []
        for attempt in range(3):
            v1, v2 = self.variants(*self.fund())
            other, _ = self.variants(*self.fund())
            h0 = self.nodes[0].getblockcount()
            self.wait_height(h0 + 1)          # just after a block: SPACING seconds before the next one
            leader, height = self.leader_of_next()
            cut = [i for i in range(3) if i != leader][0]
            pairs = [(min(cut, j), max(cut, j)) for j in range(3) if j != cut]
            for a, b in pairs:
                self.disconnect_nodes(a, b)
            if second_lock:
                self.restart_node(cut, self.extra_args[cut] + ["-posgossipnobodycheck"])
            relay_in = self.nodes[leader].add_p2p_connection(PosRelay())
            relay_out = self.nodes[cut].add_p2p_connection(PosRelay())
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())
            logs = [len(self.log_lines(i)) for i in range(3)]

            def proposed():
                for line in self.log_lines(leader)[logs[leader]:]:
                    m = re.search(r"proposing block ([0-9a-f]{64}) at height %d" % height, line)
                    if m:
                        return m.group(1)
                return None
            self.wait_until(lambda: proposed() is not None, timeout=60)
            bhash = proposed()
            relay_in.send_message(RAW["getposprop"](bytes.fromhex(bhash)[::-1]))
            self.wait_until(lambda: relay_in.got.get("posproposal"), timeout=30)
            genuine = relay_in.got["posproposal"][-1]
            forged = forge(genuine, v1, v2, other)
            placed = v1.serialize() in genuine and forged != genuine
            relay_out.answer = genuine        # asked for the body again, the relay gives the genuine one
            relay_out.send_message(RAW["posproposal"](forged))
            relay_out.sync_with_ping()
            time.sleep(0.5)
            # Nothing of the forged body is relayed: no compact proposal carrying the leader's header (its merkle
            # root, bytes 36 to 68 of the body) leaves the member before the genuine body reaches it.
            relayed_forged = len([c for c in relay_out.got.get("poscmpctprop", []) if genuine[36:68] in c])
            if not second_lock:
                relay_out.send_message(RAW["posproposal"](genuine))
                relay_out.sync_with_ping()
            # With the check switched off the genuine body is never sent unasked: only the round's fetch again, to
            # the relay that answers with it, can bring it.

            def new_lines():
                return self.log_lines(cut)[logs[cut]:]

            def signed():
                return any("signed %s at height %d" % (bhash, height) in line for line in new_lines())
            self.wait_until(lambda: signed() or any("excluding its leader" in line for line in new_lines()), timeout=60)
            time.sleep(1)
            refused = [line for line in new_lines() if ("refused: its body does not match its header" in line or
                                                         "has a body its header does not bind" in line)]
            penalised = [line for line in new_lines() if "Misbehaving" in line and "invalid posproposal" in line]
            excluded = [line for line in new_lines() if "excluding its leader" in line]
            row = (attempt, leader, cut, height, placed, len(refused), len(penalised), len(excluded), signed(),
                   relayed_forged)
            attempts.append(row)
            self.log.info("    attempt %d: leader node %d, cut-off member node %d, height %d, forged body placed %s; "
                          "refused %d, sender penalised %d, exclusions %d, leader's block signed %s, compact relays of "
                          "the forged body %d", *row)
            relay_in.peer_disconnect()
            relay_out.peer_disconnect()
            if second_lock:
                self.restart_node(cut, self.extra_args[cut])
            for a, b in pairs:
                self.connect_nodes(a, b)
            self.wait_height(height)
            self.sync_blocks()
            if not placed:
                continue
            assert_equal(excluded, [])
            assert_equal(relayed_forged, 0)
            assert refused, row
            assert penalised or second_lock, row
            assert signed(), "the cut-off member did not sign the leader's block %s" % bhash
            blk = self.nodes[0].getblock(self.nodes[0].getblockhash(height), 2)
            assert [t for t in blk["tx"] if t["txid"] == v1.hash and t["hash"] == v1.getwtxid()], \
                "the leader's block, with its transaction, did not take the height"
            return
        raise AssertionError("the case was not reached: %s" % attempts)

    def first_announcer_lies(self):
        attempts = []
        for attempt in range(3):
            v1, v2 = self.variants(*self.fund())
            h0 = self.nodes[0].getblockcount()
            self.wait_height(h0 + 1)          # just after a block: SPACING seconds before the next one
            leader, height = self.leader_of_next()
            cut = [i for i in range(3) if i != leader][0]
            pairs = [(min(cut, j), max(cut, j)) for j in range(3) if j != cut]
            for a, b in pairs:
                self.disconnect_nodes(a, b)
            relay_in = self.nodes[leader].add_p2p_connection(PosRelay())
            liar = self.nodes[cut].add_p2p_connection(PosRelay())
            honest = self.nodes[cut].add_p2p_connection(PosRelay())
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())
            self.nodes[cut].sendrawtransaction(v2.serialize().hex())   # its rebuild will not match
            logs = [len(self.log_lines(i)) for i in range(3)]

            def proposed():
                for line in self.log_lines(leader)[logs[leader]:]:
                    m = re.search(r"proposing block ([0-9a-f]{64}) at height %d" % height, line)
                    if m:
                        return m.group(1)
                return None
            self.wait_until(lambda: proposed() is not None and relay_in.got.get("poscmpctprop"), timeout=60)
            bhash = proposed()
            relay_in.send_message(RAW["getposprop"](bytes.fromhex(bhash)[::-1]))
            self.wait_until(lambda: relay_in.got.get("posproposal"), timeout=30)
            genuine = relay_in.got["posproposal"][-1]
            # The leader relays other members' compact proposals too: take the one with its own header (its merkle
            # root, bytes 36 to 68 of the body).
            self.wait_until(lambda: any(genuine[36:68] in c for c in relay_in.got.get("poscmpctprop", [])), timeout=30)
            compact = [c for c in relay_in.got["poscmpctprop"] if genuine[36:68] in c][-1]
            liar.answer = genuine.replace(v1.serialize(), v2.serialize())
            honest.answer = genuine
            placed = v1.serialize() in genuine and self.nodes[cut].getmempoolentry(v1.hash)["wtxid"] == v2.getwtxid()
            t0 = time.time()
            liar.send_message(RAW["poscmpctprop"](compact))
            liar.sync_with_ping()
            honest.send_message(RAW["poscmpctprop"](compact))
            honest.sync_with_ping()

            def new_lines():
                return self.log_lines(cut)[logs[cut]:]

            def signed():
                return any("signed %s at height %d" % (bhash, height) in line for line in new_lines())
            try:
                self.wait_until(signed, timeout=8)
                took = time.time() - t0
            except AssertionError:
                took = None
            penalised = [line for line in new_lines() if "Misbehaving" in line and "invalid posproposal" in line]
            excluded = [line for line in new_lines() if "excluding its leader" in line]
            row = (attempt, leader, cut, height, placed, len(liar.got.get("getposprop", [])),
                   len(honest.got.get("getposprop", [])), len(penalised), len(excluded), took)
            attempts.append(row)
            self.log.info("    attempt %d: leader node %d, cut-off member node %d, height %d, placed %s; body asked of the "
                          "liar %d, of the honest relay %d; penalised %d, exclusions %d; signed after %s s", *row)
            for peer in (relay_in, liar, honest):
                peer.peer_disconnect()
            for a, b in pairs:
                self.connect_nodes(a, b)
            self.wait_height(height)
            self.sync_blocks()
            if not placed:
                continue
            assert_equal(excluded, [])
            assert penalised, row
            assert took is not None and took < 5, "the member did not sign within 5 s: %s" % (row,)
            return
        raise AssertionError("the case was not reached: %s" % attempts)

    def stale_parent(self):
        h0 = self.nodes[0].getblockcount()
        self.wait_height(h0 + 1)              # just after a block: SPACING seconds before the next one
        leader, height = self.leader_of_next()
        member, other = [i for i in range(3) if i != leader]
        for j in (leader, member):
            self.disconnect_nodes(min(other, j), max(other, j))
        self.restart_node(member, self.extra_args[member] + ["-posgossipstaleonce"])
        self.connect_nodes(min(leader, member), max(leader, member))
        logs = [len(self.log_lines(i)) for i in range(3)]
        self.wait_until(lambda: self.nodes[leader].getblockcount() >= height and
                        self.nodes[member].getblockcount() >= height, timeout=120)
        proposed = set()
        for line in self.log_lines(leader)[logs[leader]:]:
            m = re.search(r"proposing block ([0-9a-f]{64}) at height %d" % height, line)
            if m:
                proposed.add(m.group(1))
        new = self.log_lines(member)[logs[member]:]
        stale = [line for line in new if "no longer builds on the tip; dropped as stale" in line]
        excluded = [line for line in new if "excluding its leader" in line]
        got = self.nodes[member].getblockhash(height)
        self.log.info("    leader node %d, member node %d, node %d cut off; stale drops %d, exclusions %d; block at %d %s, "
                      "one the leader proposed: %s", leader, member, other, len(stale), len(excluded), height, got,
                      got in proposed)
        self.restart_node(member, self.extra_args[member])
        for a, b in ((0, 1), (0, 2), (1, 2)):
            self.connect_nodes(a, b)
        self.sync_blocks()
        assert stale, "the forced stale path was not taken"
        assert_equal(excluded, [])
        assert got in proposed, "the block at %d is not one the leader proposed" % height

    def flood_keeps_the_fetch_book(self):
        attempts = []
        for attempt in range(3):
            v1, v2 = self.variants(*self.fund())
            h0 = self.nodes[0].getblockcount()
            self.wait_height(h0 + 1)          # just after a block: SPACING seconds before the next one
            leader, height = self.leader_of_next()
            cut = [i for i in range(3) if i != leader][0]
            pairs = [(min(cut, j), max(cut, j)) for j in range(3) if j != cut]
            for a, b in pairs:
                self.disconnect_nodes(a, b)
            relay_in = self.nodes[leader].add_p2p_connection(PosRelay())
            silent = self.nodes[cut].add_p2p_connection(PosRelay())
            honest = self.nodes[cut].add_p2p_connection(PosRelay())
            flooder = self.nodes[cut].add_p2p_connection(PosRelay())
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())
            self.nodes[cut].sendrawtransaction(v2.serialize().hex())   # its rebuild will not match
            logs = [len(self.log_lines(i)) for i in range(3)]

            def proposed():
                for line in self.log_lines(leader)[logs[leader]:]:
                    m = re.search(r"proposing block ([0-9a-f]{64}) at height %d" % height, line)
                    if m:
                        return m.group(1)
                return None
            self.wait_until(lambda: proposed() is not None, timeout=60)
            bhash = proposed()
            relay_in.send_message(RAW["getposprop"](bytes.fromhex(bhash)[::-1]))
            self.wait_until(lambda: relay_in.got.get("posproposal"), timeout=30)
            genuine = relay_in.got["posproposal"][-1]
            self.wait_until(lambda: any(genuine[36:68] in c for c in relay_in.got.get("poscmpctprop", [])), timeout=30)
            compact = [c for c in relay_in.got["poscmpctprop"] if genuine[36:68] in c][-1]
            honest.answer = genuine
            placed = v1.serialize() in genuine and self.nodes[cut].getmempoolentry(v1.hash)["wtxid"] == v2.getwtxid()
            t0 = time.time()
            silent.send_message(RAW["poscmpctprop"](compact))
            silent.sync_with_ping()
            honest.send_message(RAW["poscmpctprop"](compact))
            honest.sync_with_ping()
            # Compacts with made-up parents (bytes 4 to 36): not on the tip, so ignored without a penalty, but each
            # announces a hash of its own before anything is checked.
            for k in range(1001):
                flooder.send_message(RAW["poscmpctprop"](compact[:4] + hashlib.sha256(b"%d" % k).digest() + compact[36:]))
            flooder.sync_with_ping(timeout=120)
            t_flood = time.time() - t0

            def new_lines():
                return self.log_lines(cut)[logs[cut]:]

            def signed():
                return any("signed %s at height %d" % (bhash, height) in line for line in new_lines())
            try:
                self.wait_until(signed, timeout=8)
                took = time.time() - t0
            except AssertionError:
                took = None
            excluded = [line for line in new_lines() if "excluding its leader" in line]
            row = (attempt, leader, cut, height, placed, round(t_flood, 2), len(silent.got.get("getposprop", [])),
                   len(honest.got.get("getposprop", [])), len(excluded), took)
            attempts.append(row)
            self.log.info("    attempt %d: leader node %d, cut-off member node %d, height %d, placed %s; flood sent in %s s; "
                          "body asked of the silent announcer %d, of the honest one %d; exclusions %d; signed after %s s",
                          *row)
            for peer in (relay_in, silent, honest, flooder):
                peer.peer_disconnect()
            for a, b in pairs:
                self.connect_nodes(a, b)
            self.wait_height(height)
            self.sync_blocks()
            if not placed:
                continue
            assert_equal(excluded, [])
            assert took is not None, "the member did not sign the leader's block after the flood: %s" % (row,)
            return
        raise AssertionError("the case was not reached: %s" % attempts)

    @staticmethod
    def header_parts(body, compact):
        """Split a full proposal into (header before the proof's solution, solution, the rest): the header is the
        common prefix of the full and the compact proposal; it ends with the challenge and the solution."""
        def compact_size(b, i):
            n = b[i]
            if n < 253:
                return n, i + 1
            if n == 253:
                return int.from_bytes(b[i + 1:i + 3], "little"), i + 3
            return int.from_bytes(b[i + 1:i + 5], "little"), i + 5
        for fixed in (76, 80, 112, 116):
            try:
                n, i = compact_size(body, fixed)
                j = i + n                                   # past the challenge
                m, k = compact_size(body, j)
                end = k + m                                 # past the solution
            except IndexError:
                continue
            if body[:end] == compact[:end] and compact[end:end + 60] == body[end + 1:end + 61]:
                return body[:j], body[k:end], body[end:]
        raise AssertionError("could not find the header's solution")

    @staticmethod
    def with_solution(prefix, solution, rest):
        n = len(solution)
        size = bytes([n]) if n < 253 else (b"\xfd" + n.to_bytes(2, "little") if n < 65536 else b"\xfe" + n.to_bytes(4, "little"))
        return prefix + size + solution + rest

    def solution_appended_small(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, sol + b"\x00", rest))

    def solution_appended_large(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, sol + b"\x00" * 450_000, rest))

    @staticmethod
    def reencoded(sol, kind):
        """The staging solution (one push of a DER signature) with the signature re-encoded as `kind`, still one
        push and still a signature that CPubKey::Verify's lax DER parsing accepts."""
        assert sol[0] <= 75 and len(sol) == 1 + sol[0], sol.hex()
        sig = sol[1:]
        rl = sig[3]
        r = sig[4:4 + rl]
        sl = sig[5 + rl]
        s = sig[6 + rl:6 + rl + sl]
        pad = 420_000
        if kind == "padded_r":
            new = b"\x30\x80" + b"\x02\x83" + (pad + rl).to_bytes(3, "big") + b"\x00" * pad + r + b"\x02" + bytes([sl]) + s
        elif kind == "padded_s":
            new = b"\x30\x80" + b"\x02" + bytes([rl]) + r + b"\x02\x83" + (pad + sl).to_bytes(3, "big") + b"\x00" * pad + s
        elif kind == "signature_73_bytes":
            r2 = b"\x00" * max(1, 73 - len(sig)) + r
            inner = b"\x02" + bytes([len(r2)]) + r2 + b"\x02" + bytes([sl]) + s
            new = b"\x30" + bytes([len(inner)]) + inner
        elif kind == "high_s":
            v = SECP256K1_ORDER - int.from_bytes(s, "big")
            s2 = v.to_bytes((v.bit_length() + 7) // 8, "big")
            s2 = b"\x00" + s2 if s2[0] & 0x80 else s2
            inner = b"\x02" + bytes([rl]) + r + b"\x02" + bytes([len(s2)]) + s2
            new = b"\x30" + bytes([len(inner)]) + inner
        else:
            raise AssertionError(kind)
        return bytes(CScript([new]))

    def padded_r(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, self.reencoded(sol, "padded_r"), rest),
                                 encoding=True)

    def padded_s(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, self.reencoded(sol, "padded_s"), rest),
                                 encoding=True)

    def signature_73_bytes(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, self.reencoded(sol, "signature_73_bytes"), rest),
                                 encoding=True)

    def high_s(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, self.reencoded(sol, "high_s"), rest),
                                 encoding=True)

    def padded_signature_reaches_the_round(self):
        self.altered_header_body(lambda prefix, sol, rest: self.with_solution(prefix, self.reencoded(sol, "padded_r"), rest),
                                 checks_off=True)

    def heavy_block_signed(self):
        """Transactions filling more than two blocks are sent: a block built from them is at the weight limit, its
        leader's proposal, signed by the members, and nobody is excluded."""
        # Independent transactions (a chain would pass the mempool's descendant limit), each spending one
        # confirmed output of a fan-out: 30 of about 40,000 weight (215 outputs) and 40 of about 4,700 (25
        # outputs) that fill what the large ones leave; 1.4 million weight in all.
        sizes = [215] * 30 + [25] * 40
        gtx, gn, gval = self.genesis_coin
        fan = CTransaction()
        fan.nVersion = 2
        fan.vin = [CTxIn(COutPoint(int(gtx, 16), gn))]
        part, fee, n = 10_000_000, 500_000, len(sizes)
        fan.vout = [CTxOut(part, CScript([OP_TRUE])) for _ in range(n)]
        fan.vout += [CTxOut(gval - n * part - fee, CScript([OP_TRUE])), CTxOut(fee)]
        fan.rehash()
        self.nodes[0].sendrawtransaction(fan.serialize().hex())
        self.wait_until(lambda: all(node.gettxout(fan.hash, n, False) is not None for node in self.nodes), timeout=120)
        self.genesis_coin = (fan.hash, n, gval - n * part - fee)
        chain = []
        for i, k in enumerate(sizes):
            tx = CTransaction()
            tx.nVersion = 2
            tx.vin = [CTxIn(COutPoint(int(fan.hash, 16), i))]
            each = 10_000
            fee = 4 * k * 200   # the same fee rate for both sizes
            tx.vout = [CTxOut(each, CScript([OP_TRUE])) for _ in range(k)]
            tx.vout += [CTxOut(part - each * k - fee, CScript([OP_TRUE])), CTxOut(fee)]
            tx.rehash()
            chain.append(tx)
        order = [t for pair in zip(chain[:30], chain[30:60]) for t in pair] + chain[60:]
        logs = [len(self.log_lines(i)) for i in range(3)]

        def send_all(node):   # to every node at once: relay would leave a leader building from part of them
            for tx in order:
                try:
                    node.sendrawtransaction(tx.serialize().hex())
                except JSONRPCException as e:
                    assert e.error["code"] == -27, e.error   # already known or mined
        threads = [threading.Thread(target=send_all, args=(node,)) for node in self.nodes]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        hashes = {c.hash for c in chain}
        h = self.nodes[0].getblockcount()
        rows = []
        for height in (h + 1, h + 2, h + 3):   # more than two blocks' worth after sending: one is full
            self.wait_height(height)
            self.sync_blocks()
            blk = self.nodes[0].getblock(self.nodes[0].getblockhash(height), 1)
            held = len([t for t in blk["tx"] if t in hashes])
            leader = [i for i in range(3) if any("proposing block %s at height %d" % (blk["hash"], height) in line
                                                 for line in self.log_lines(i)[logs[i]:])]
            signed = [i for i in range(3) if i not in leader and any("signed %s at height %d" % (blk["hash"], height) in line
                                                                      for line in self.log_lines(i)[logs[i]:])]
            rows.append((height, blk["weight"], held, leader, signed))
            self.log.info("    height %d: block weight %d with %d of the heavy transactions; proposed by %s; "
                          "signed by members %s", *rows[-1])
        excluded = [line for i in range(3) for line in self.log_lines(i)[logs[i]:] if "excluding its leader" in line]
        self.wait_until(lambda: not self.nodes[0].getrawmempool(), timeout=180)
        self.sync_blocks()
        assert_equal(excluded, [])
        full = [r for r in rows if r[1] > 385_000]
        assert full, "no block at the weight limit: %s" % rows
        assert all(r[3] and r[4] for r in full), rows

    def altered_header_body(self, alter, encoding=False, checks_off=False):
        """The leader's full proposal with its staging solution altered, sent unasked to a cut-off member, then the
        genuine one: the sender is penalised, nobody excluded, the genuine one signed. With `encoding`, the refusal
        names the signature's encoding. With `checks_off` the member's checks of a body are switched off
        (-posgossipnobodycheck): the altered body reaches the round, nobody is excluded and the leader's block is
        signed."""
        attempts = []
        for attempt in range(3):
            v1, v2 = self.variants(*self.fund())
            h0 = self.nodes[0].getblockcount()
            self.wait_height(h0 + 1)
            leader, height = self.leader_of_next()
            cut = [i for i in range(3) if i != leader][0]
            pairs = [(min(cut, j), max(cut, j)) for j in range(3) if j != cut]
            for a, b in pairs:
                self.disconnect_nodes(a, b)
            if checks_off:
                self.restart_node(cut, self.extra_args[cut] + ["-posgossipnobodycheck"])
            relay_in = self.nodes[leader].add_p2p_connection(PosRelay())
            relay_out = self.nodes[cut].add_p2p_connection(PosRelay())
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())
            logs = [len(self.log_lines(i)) for i in range(3)]

            def proposed():
                for line in self.log_lines(leader)[logs[leader]:]:
                    m = re.search(r"proposing block ([0-9a-f]{64}) at height %d" % height, line)
                    if m:
                        return m.group(1)
                return None
            self.wait_until(lambda: proposed() is not None, timeout=60)
            bhash = proposed()
            relay_in.send_message(RAW["getposprop"](bytes.fromhex(bhash)[::-1]))
            self.wait_until(lambda: relay_in.got.get("posproposal"), timeout=30)
            genuine = relay_in.got["posproposal"][-1]
            self.wait_until(lambda: any(genuine[36:68] in c for c in relay_in.got.get("poscmpctprop", [])), timeout=30)
            compact = [c for c in relay_in.got["poscmpctprop"] if genuine[36:68] in c][-1]
            prefix, sol, rest = self.header_parts(genuine, compact)
            altered = alter(prefix, sol, rest)
            if checks_off:
                relay_out.answer = altered   # asked again, the relay answers with the altered body too
            placed = altered != genuine
            relay_out.send_message(RAW["posproposal"](altered))
            relay_out.sync_with_ping(timeout=60)
            if not checks_off:   # with the checks off only the altered body reaches the member, so it decides
                relay_out.send_message(RAW["posproposal"](genuine))
                relay_out.sync_with_ping()

            def new_lines():
                return self.log_lines(cut)[logs[cut]:]

            def signed():
                return any("signed %s at height %d" % (bhash, height) in line for line in new_lines())
            try:
                self.wait_until(lambda: signed() or any("excluding its leader" in line for line in new_lines()), timeout=30)
            except AssertionError:
                pass
            time.sleep(1)
            penalised = [line for line in new_lines() if "Misbehaving" in line and "invalid posproposal" in line]
            excluded = [line for line in new_lines() if "excluding its leader" in line]
            by_encoding = [line for line in new_lines() if "its leader signature is not in its one encoding" in line]
            row = (attempt, leader, cut, height, len(sol), len(altered) - len(genuine), len(penalised), len(by_encoding),
                   len(excluded), signed())
            attempts.append(row)
            self.log.info("    attempt %d: leader node %d, cut-off member node %d, height %d; solution of %d bytes, "
                          "%+d bytes; sender penalised %d, refused for the encoding %d, exclusions %d, leader's block "
                          "signed %s", *row)
            relay_in.peer_disconnect()
            relay_out.peer_disconnect()
            if checks_off:
                self.restart_node(cut, self.extra_args[cut])
            for a, b in pairs:
                self.connect_nodes(a, b)
            self.wait_height(height)
            self.sync_blocks()
            if not placed:
                continue
            assert_equal(excluded, [])
            assert signed(), row
            if checks_off:
                assert_equal(by_encoding, [])
            else:
                assert penalised, row
            if encoding:
                assert by_encoding, row
            return
        raise AssertionError("the case was not reached: %s" % attempts)

    def bad_signature_keeps_the_fetch_open(self):
        attempts = []
        for attempt in range(3):
            v1, v2 = self.variants(*self.fund())
            h0 = self.nodes[0].getblockcount()
            self.wait_height(h0 + 1)
            leader, height = self.leader_of_next()
            cut = [i for i in range(3) if i != leader][0]
            pairs = [(min(cut, j), max(cut, j)) for j in range(3) if j != cut]
            for a, b in pairs:
                self.disconnect_nodes(a, b)
            relay_in = self.nodes[leader].add_p2p_connection(PosRelay())
            forger = self.nodes[cut].add_p2p_connection(PosRelay())
            honest = self.nodes[cut].add_p2p_connection(PosRelay())
            self.nodes[leader].sendrawtransaction(v1.serialize().hex())   # the cut-off member never gets it
            logs = [len(self.log_lines(i)) for i in range(3)]

            def proposed():
                for line in self.log_lines(leader)[logs[leader]:]:
                    m = re.search(r"proposing block ([0-9a-f]{64}) at height %d" % height, line)
                    if m:
                        return m.group(1)
                return None
            self.wait_until(lambda: proposed() is not None, timeout=60)
            bhash = proposed()
            relay_in.send_message(RAW["getposprop"](bytes.fromhex(bhash)[::-1]))
            self.wait_until(lambda: relay_in.got.get("posproposal"), timeout=30)
            genuine = relay_in.got["posproposal"][-1]
            self.wait_until(lambda: any(genuine[36:68] in c for c in relay_in.got.get("poscmpctprop", [])), timeout=30)
            compact = [c for c in relay_in.got["poscmpctprop"] if genuine[36:68] in c][-1]
            prefix, sol, rest = self.header_parts(genuine, compact)
            bad = sol[:-1] + bytes([sol[-1] ^ 1])                 # one byte of the signature changed
            forged = self.with_solution(prefix, bad, rest)
            honest.answer = genuine
            placed = v1.hash not in self.nodes[cut].getrawmempool()
            forger.send_message(RAW["posproposal"](forged))
            forger.sync_with_ping()
            t0 = time.time()
            honest.send_message(RAW["poscmpctprop"](compact))
            honest.sync_with_ping()

            def new_lines():
                return self.log_lines(cut)[logs[cut]:]

            def signed():
                return any("signed %s at height %d" % (bhash, height) in line for line in new_lines())
            try:
                self.wait_until(signed, timeout=8)
                took = time.time() - t0
            except AssertionError:
                took = None
            penalised = [line for line in new_lines() if "Misbehaving" in line and "invalid posproposal" in line]
            row = (attempt, leader, cut, height, placed, len(penalised), len(honest.got.get("getposprop", [])), took)
            attempts.append(row)
            self.log.info("    attempt %d: leader node %d, cut-off member node %d, height %d, member lacks the tx %s; "
                          "forger penalised %d; body asked of the honest announcer %d; signed after %s s", *row)
            for peer in (relay_in, forger, honest):
                peer.peer_disconnect()
            for a, b in pairs:
                self.connect_nodes(a, b)
            self.wait_height(height)
            self.sync_blocks()
            if not placed:
                continue
            assert penalised, row
            assert took is not None, "the member did not fetch and sign after the forged body: %s" % (row,)
            return
        raise AssertionError("the case was not reached: %s" % attempts)


if __name__ == '__main__':
    PosProposalWitnessTest().main()
