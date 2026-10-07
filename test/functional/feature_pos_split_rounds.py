#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Split payouts, second generation: rounds paid a bucket at a time.

The first generation paid every delegator of a pot in one transaction, so a
pool's size was bounded by what a block holds (audit A11). From
-posrecordsv2height the pots of one epoch become a ROUND, shared among everyone
who stood behind the pool when the epoch began, in buckets of about 32
delegators; any claim pays one bucket of each round it touches, and the round's
bitmap records which are paid.

What this asserts, with 40 delegators (two buckets):
 - a claim for one delegator makes the round and pays exactly its bucket, each
   member its floor-division share of 99% of the round;
 - a second claim pays the other bucket and closes the round, its remainder
   going back into a pot; every eligible delegator is paid once, exactly;
 - a delegator who joined during the epoch gets nothing from its round;
 - a hand-built spend of a round that pays the wrong amounts is refused;
 - rounds are a pure function of the UTXO set: they survive a restart.
"""

from decimal import Decimal

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, hash160
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

UNBONDING = 5
NOTICE = 10
EPOCH = 20
COIN = 100_000_000
DELEGATORS = 40
LEND = 10                      # SEQ each delegator lends
OWN = 100                      # the pool's own config stake, SEQ


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def p2wpkh_hex(pubkey_hex):
    return "0014" + hash160(bytes.fromhex(pubkey_hex)).hex()


def parse_round(spk_hex):
    """(signer, epoch, weight, distributable, buckets, paid bitmap) of a round
    script, or None. Data: epoch(4) weight(8) signer weight(8)
    distributable(8) buckets(2) salt(32) bitmap."""
    b = bytes.fromhex(spk_hex)
    if not b.startswith(b"\x06SEQRND\x75\x21"):
        return None
    signer = b[9:42].hex()
    at = 43
    n = b[at]
    at += 1
    if n == 0x4c:
        n = b[at]
        at += 1
    data = b[at:at + n]
    epoch = int.from_bytes(data[0:4], 'little')
    weight = int.from_bytes(data[4:12], 'little')
    dist = int.from_bytes(data[20:28], 'little')
    buckets = int.from_bytes(data[28:30], 'little')
    paid = data[62:]
    return signer, epoch, weight, dist, buckets, paid


class PosSplitRoundsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_wif, self.a_pub = make_staker()
        self.extra_args = [[
            "-con_pos=1",
            "-poshardeningheight=0",   # the split record is funded from an OP_TRUE coin
            "-posvrf=1", "-posunbonding=%d" % UNBONDING, "-posslotinterval=1",
            "-pospayoutnotice=%d" % NOTICE, "-possplitepoch=%d" % EPOCH,
            "-signblockscript=51", "-initialfreecoins=1000000000000", "-anyonecanspendaremine=1",
            "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1",
            "-staker=%s:%d" % (self.a_pub, OWN * COIN), "-validatepegin=0", "-txindex=1",
            "-acceptnonstdtxn=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, n=1):
        for _ in range(n):
            self.nodes[0].generateposblock(self.a_wif)

    def find_free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    if node.gettxout(tx['txid'], vout['n']):
                        return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no unspent OP_TRUE genesis output")

    def paid_in(self, txid):
        """atoms paid to each P2WPKH script, and round / pot outputs, of a claim."""
        tx = self.nodes[0].getrawtransaction(txid, True)
        paid, rounds, pots = {}, [], 0
        for n, v in enumerate(tx["vout"]):
            spk = v["scriptPubKey"]["hex"]
            atoms = int(round(Decimal(str(v.get("value", 0))) * COIN)) if v.get("value") else 0
            r = parse_round(spk)
            if r:
                rounds.append((n, atoms, r))
            elif spk.startswith("06" + b"SEQPOT".hex()):
                pots += atoms
            else:
                paid[spk] = paid.get(spk, 0) + atoms
        return paid, rounds, pots

    def run_test(self):
        n0 = self.nodes[0]
        w0 = n0.get_wallet_rpc(self.default_wallet_name)
        self.mine(1)

        self.log.info("A split pool with %d delegators", DELEGATORS)
        activation = n0.getblockcount() + NOTICE + 5
        rec = n0.getpayoutscript(self.a_pub, activation, "split", None, 0)
        w0_spk = bytes.fromhex(w0.getaddressinfo(w0.getnewaddress())["scriptPubKey"])
        txid, voutn, in_amount = self.find_free_coin(n0)
        fund = CTransaction()
        fund.nVersion = 2
        fund.vin = [CTxIn(COutPoint(int(txid, 16), voutn))]
        fee = 100_000
        endowment = 5000 * COIN
        fund.vout = [
            CTxOut(1_000_000, bytes.fromhex(rec["script"])),
            CTxOut(endowment, w0_spk),
            CTxOut(in_amount - 1_000_000 - endowment - fee, CScript([0x51])),
            CTxOut(fee),
        ]
        n0.sendrawtransaction(fund.serialize().hex())
        self.mine(1)
        controllers = []
        for i in range(DELEGATORS):
            controllers.append(w0.delegatestake(self.a_pub, LEND)["controller"])
            if i % 10 == 9:
                self.mine(1)
        self.mine(1)
        assert_equal(len(set(controllers)), DELEGATORS)
        assert_equal(n0.getstakerinfo()[self.a_pub], (OWN + DELEGATORS * LEND) * COIN)

        self.log.info("Fee blocks inside one epoch fill its pots")
        while n0.getblockcount() < activation:
            self.mine(1)
        # Start of a fresh epoch, then a few fee-paying blocks inside it.
        while (n0.getblockcount() + 1) % EPOCH != 0:
            self.mine(1)
        self.mine(1)
        epoch = n0.getblockcount() // EPOCH
        n0.createwallet("late")
        wl = n0.get_wallet_rpc("late")
        w0.settxfee(Decimal("1"))
        w0.sendtoaddress(address=wl.getnewaddress(), amount=100, fee_asset_label="bitcoin")
        self.mine(1)
        self.log.info("A delegator who joins during the epoch")
        late = wl.delegatestake(self.a_pub, LEND)["controller"]
        self.mine(1)
        for _ in range(3):
            w0.sendtoaddress(address=w0.getnewaddress(), amount=1, fee_asset_label="bitcoin")
            self.mine(1)
        assert_equal(n0.getblockcount() // EPOCH, epoch)
        w0.settxfee(0)
        self.mine(100)  # coinbase maturity

        self.log.info("A claim for one delegator makes the round and pays its bucket")
        first = controllers[0]
        claim1 = w0.claimpoolrewards(self.a_pub, first)
        assert_equal(claim1["rounds"], 1)
        paid1, rounds1, _ = self.paid_in(claim1["txid"])
        assert_equal(len(rounds1), 1)
        _, round_atoms, (signer, r_epoch, weight, dist, buckets, bitmap) = rounds1[0]
        assert_equal(signer, self.a_pub)
        assert_equal(r_epoch, epoch)
        assert_equal(buckets, 2)
        assert_equal(weight, (OWN + DELEGATORS * LEND) * COIN)
        assert_equal(bin(bitmap[0]).count("1"), 1)
        share = {}
        for k, w in [(self.a_pub, OWN)] + [(c, LEND) for c in controllers]:
            share[p2wpkh_hex(k)] = (dist - dist // 100) * w * COIN // weight
        assert p2wpkh_hex(first) in paid1
        for spk, atoms in paid1.items():
            if spk in share:
                assert_equal(atoms, share[spk])
        assert p2wpkh_hex(late) not in paid1
        self.mine(1)

        self.log.info("Its bucket is paid: a second claim for the same delegator finds nothing")
        assert_raises_rpc_error(-4, "nothing to pay", w0.claimpoolrewards, self.a_pub, first)

        self.log.info("A claim for a delegator in the other bucket closes the round")
        other = next(c for c in controllers if p2wpkh_hex(c) not in paid1)
        claim2 = w0.claimpoolrewards(self.a_pub, other)
        paid2, rounds2, repotted = self.paid_in(claim2["txid"])
        assert_equal(rounds2, [])
        assert_greater_than(repotted, 0)
        self.mine(1)
        everyone = dict(paid1)
        for spk, atoms in paid2.items():
            if spk in share:
                assert spk not in paid1, "a delegator was paid twice"
            everyone[spk] = everyone.get(spk, 0) + atoms
        for spk, atoms in share.items():
            assert_equal(everyone.get(spk, 0), atoms)
        assert p2wpkh_hex(late) not in everyone, "the latecomer was paid from the epoch it joined in"

        self.log.info("A spend of a round paying the wrong amounts is refused")
        w0.settxfee(Decimal("1"))
        for _ in range(2):
            w0.sendtoaddress(address=w0.getnewaddress(), amount=1, fee_asset_label="bitcoin")
            self.mine(1)
        w0.settxfee(0)
        self.mine(100)
        claim3 = w0.claimpoolrewards(self.a_pub, first)
        self.mine(1)
        _, rounds3, _ = self.paid_in(claim3["txid"])
        n, atoms, _ = rounds3[0]
        steal = CTransaction()
        steal.nVersion = 2
        steal.vin = [CTxIn(COutPoint(int(claim3["txid"], 16), n))]
        steal.vout = [CTxOut(atoms - 500, bytes.fromhex(p2wpkh_hex(other))), CTxOut(500)]
        assert_raises_rpc_error(-26, "bad-pot-claim", n0.sendrawtransaction, steal.serialize().hex())

        self.log.info("Rounds are a pure function of the UTXO set: they survive a restart")
        before = n0.listpools(self.a_pub, 0, False, True)
        self.restart_node(0)
        n0 = self.nodes[0]
        assert_equal(n0.listpools(self.a_pub, 0, False, True), before)


if __name__ == '__main__':
    PosSplitRoundsTest().main()
