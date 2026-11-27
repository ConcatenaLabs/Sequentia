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
stale_parent: a member about to sign the leader's proposal finds its parent is no longer the tip (forced with
  -posgossipstaleonce): nothing was judged, so it drops the proposal as stale and excludes nobody, and with the
  third node cut off the height is certified on a block the same leader proposed, signed by that member.
"""

import hashlib
import os
import re
import time

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, CTxOutWitness
from test_framework.p2p import MESSAGEMAP, P2PInterface
from test_framework.script import CScript, OP_0, OP_DROP, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

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
                                                  "flood_keeps_the_fetch_book"]:
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
            relay_out.send_message(RAW["posproposal"](genuine))
            relay_out.sync_with_ping()

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


if __name__ == '__main__':
    PosProposalWitnessTest().main()
