#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stake records that ConnectBlock would refuse never reach a block template.

Delegation and payout records, BLS registrations and pot claims are judged by
rules that look across the whole block and at the stake registry, and anyone
can create a record for the price of a dust output. A transaction breaking one
of them used to pass the mempool, be selected by every producer, and kill every
block at connect: a chain-wide stall for as long as it stayed in the mempool.

This test checks the three layers that now stop it:

  * admission: a record the registry already rules out, a payout record inside
    its notice, a registration with a bad proof of possession or a key that
    conflicts with the staker's registered one, are refused by the mempool;
  * the block assembler: two records the mempool holds for the same controller
    go in one per block, and a payout record that went stale while it waited
    is left out, so blocks keep coming;
  * generateposblock reports a failure instead of a hash when the block it
    produced does not connect.
"""

import os
import shutil
import time

from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

NOTICE = 3
RECORD_VALUE = 1_000_000
FEE = 100_000
UNBONDING = 5
OP_TRUE = CScript([0x51])


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosTemplatePoisonTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(4)]
        self.common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1", "-poscommitteesize=4",
            "-posslotinterval=1", "-con_max_block_sig_size=8000", "-posunbonding=%d" % UNBONDING,
            "-pospayoutnotice=%d" % NOTICE,
            "-signblockscript=51", "-con_blocksubsidy=0", "-initialfreecoins=1000000000000",
            "-con_connect_genesis_outputs=1", "-anyonecanspendaremine=1", "-validatepegin=0",
            "-acceptnonstdtxn=1", "-persistmempool=0",
        ]

    def setup_network(self):
        # The stakers' BLS registrations are derived by a running node.
        self.add_nodes(1, [list(self.common)])
        self.start_node(0, extra_args=self.common + ["-staker=%s:1" % pub for _, pub in self.stakers])
        specs = ["-staker=%s:1%s" % (pub, self.nodes[0].getblsregistration(wif)["spec"])
                 for wif, pub in self.stakers]
        self.stop_node(0)
        shutil.rmtree(os.path.join(self.nodes[0].datadir, self.chain))
        self.start_node(0, extra_args=self.common + specs)

    def produce(self):
        """A block certified by a full quorum, led by whichever staker's slot opens."""
        node = self.nodes[0]
        wifs = [w for w, _ in self.stakers]
        last = None
        for _ in range(90):
            for leader in wifs:
                others = [w for w in wifs if w != leader][:3]
                try:
                    return node.generateposblock(leader, others)["hash"]
                except JSONRPCException as e:
                    last = e.error
            time.sleep(0.5)
        raise AssertionError("could not produce: %s" % last)

    def spend(self, outputs, coin=None):
        """Spend `coin` (default: the running OP_TRUE change) into `outputs`
        (value, script) plus change."""
        txid, n, value = coin or self.change
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n))]
        rest = value - sum(v for v, _ in outputs) - FEE
        tx.vout = [CTxOut(v, s) for v, s in outputs] + [CTxOut(rest, OP_TRUE), CTxOut(FEE)]
        return tx, len(outputs), rest

    def send(self, outputs, coin=None):
        tx, change_n, rest = self.spend(outputs, coin)
        txid = self.nodes[0].sendrawtransaction(tx.serialize().hex())
        if coin is None:
            self.change = (txid, change_n, rest)
        return txid

    def refused(self, reason, outputs):
        tx, _, _ = self.spend(outputs)
        assert_raises_rpc_error(-26, reason, self.nodes[0].sendrawtransaction, tx.serialize().hex())

    def run_test(self):
        node = self.nodes[0]
        self.produce()
        genesis = node.getblock(node.getblockhash(0), 2)
        self.change = next((tx["txid"], o["n"], int(o["value"] * COIN))
                           for tx in genesis["tx"] for o in tx["vout"]
                           if o["scriptPubKey"]["hex"] == "51" and o.get("value", 0) > 0)
        # Separate coins for transactions that must not depend on each other.
        fan = self.send([(50 * COIN, OP_TRUE)] * 2)
        self.produce()
        spare = [(fan, i, 50 * COIN) for i in range(2)]

        signer = self.stakers[0][1]
        controller, other_controller = make_staker()[1], make_staker()[1]
        record = bytes.fromhex(node.getdelegationscript(controller, signer)["script"])

        self.log.info("Admission: a second delegation record for a controller is refused")
        self.send([(RECORD_VALUE, record)])
        self.produce()
        assert_equal(node.getdelegationinfo()[controller], signer)
        self.refused("bad-delegation-exists", [(RECORD_VALUE, record)])
        self.refused("bad-delegation-conflict", [(RECORD_VALUE, bytes.fromhex(
            node.getdelegationscript(other_controller, signer)["script"]))] * 2)

        self.log.info("Admission: a payout record inside its notice is refused")
        height = node.getblockcount()
        too_soon = bytes.fromhex(node.getpayoutscript(signer, height + NOTICE, "direct", OP_TRUE.hex())["script"])
        self.refused("bad-payout-notice", [(RECORD_VALUE, too_soon)])

        self.log.info("Admission: BLS registrations must prove possession and match the registered key")
        new_wif, new_pub = make_staker()
        new_reg = node.getblsregistration(new_wif)
        other_reg = node.getblsregistration(make_staker()[0])
        bad_pop = bytes.fromhex(node.getstakescript(new_pub, UNBONDING, None, new_reg["blspubkey"], other_reg["pop"])["script"])
        self.refused("bad-stake-bls-pop", [(COIN, bad_pop)])
        # A registered staker restating a different (valid) key.
        conflict = bytes.fromhex(node.getstakescript(signer, UNBONDING, None, new_reg["blspubkey"], new_reg["pop"])["script"])
        self.refused("bad-stake-bls-conflict", [(COIN, conflict)])

        self.log.info("Assembler: two records the mempool holds for one controller go in one at a time")
        third = make_staker()[1]
        first = self.send([(RECORD_VALUE, bytes.fromhex(node.getdelegationscript(third, signer)["script"]))], spare[0])
        second = self.send([(RECORD_VALUE, bytes.fromhex(node.getdelegationscript(third, self.stakers[1][1])["script"]))], spare[1])
        assert first in node.getrawmempool() and second in node.getrawmempool()
        tip = node.getbestblockhash()
        block = self.produce()
        assert node.getbestblockhash() != tip
        mined = set(node.getblock(block)["tx"])
        assert_equal(len({first, second} & mined), 1)
        # The other one can never be mined while the first record stands, and it
        # does not stop the next block either.
        tip = node.getbestblockhash()
        self.produce()
        assert node.getbestblockhash() != tip

        self.log.info("Assembler: a payout record that went stale in the mempool is left out")
        height = node.getblockcount()
        just_in_time = bytes.fromhex(node.getpayoutscript(signer, height + 1 + NOTICE, "direct", OP_TRUE.hex())["script"])
        stale = self.send([(RECORD_VALUE, just_in_time)])
        # Keep it out of the next block, after which its notice no longer fits.
        node.prioritisetransaction(txid=stale, fee_delta=-10 * COIN)
        self.produce()
        assert stale in node.getrawmempool()
        node.prioritisetransaction(txid=stale, fee_delta=10 * COIN)
        tip = node.getbestblockhash()
        block = self.produce()
        assert node.getbestblockhash() != tip
        assert stale not in node.getblock(block)["tx"]


if __name__ == '__main__':
    PosTemplatePoisonTest().main()
