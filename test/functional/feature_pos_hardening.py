#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The audit hardening rules, on both sides of -poshardeningheight.

A delegation record re-points a controller's stake weight and a payout record
redirects what a signer's blocks earn. Both used to take effect merely by
existing, so anyone could create one naming any key. From the hardening height:

  * a record must be created by a transaction spending a coin only its key can
    spend (here a P2PK output of the key);
  * a delegation to the controller itself is refused;
  * a record may not be spent and re-created identically in one block;
  * the payout record in force may not be spent: ending a policy takes the
    same notice as starting one (a superseded or still-pending record may go).

Below the height the old rules still apply, so history produced before the
cutover stays valid.
"""

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, LegacySignatureHash, OP_CHECKSIG, SIGHASH_ALL
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

COIN = 100_000_000
FEE = 100_000
RECORD_VALUE = 1_000_000
UNBONDING = 5
NOTICE = 3
HARDENING_HEIGHT = 12
OP_TRUE = CScript([0x51])


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return k, byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosHardeningTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.a_key, self.a_wif, self.a_pub = make_key()   # producer
        self.c_key, self.c_wif, self.c_pub = make_key()   # a controller
        self.p_key, self.p_wif, self.p_pub = make_key()   # a pool
        self.extra_args = [[
            "-con_pos=1", "-posvrf=1", "-posunbonding=%d" % UNBONDING, "-posslotinterval=1",
            "-pospayoutnotice=%d" % NOTICE, "-poshardeningheight=%d" % HARDENING_HEIGHT,
            "-signblockscript=51", "-initialfreecoins=1000000000000", "-anyonecanspendaremine=0",
            "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1", "-acceptnonstdtxn=1",
            "-staker=%s:%d" % (self.a_pub, COIN), "-validatepegin=0", "-persistmempool=0",
        ]]

    def free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no OP_TRUE genesis output")

    def build(self, inputs, outs):
        """inputs: [(txid, n, value, key-or-None, script-or-None)]; signs keyed inputs."""
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n), nSequence=0xfffffffe) for txid, n, _, _, _ in inputs]
        rest = sum(v for _, _, v, _, _ in inputs) - sum(v for v, _ in outs) - FEE
        tx.vout = [CTxOut(v, s) for v, s in outs] + [CTxOut(rest, OP_TRUE), CTxOut(FEE)]
        for i, (_, _, _, key, script) in enumerate(inputs):
            if key is None:
                continue
            sighash, err = LegacySignatureHash(CScript(script), tx, i, SIGHASH_ALL)
            assert err is None
            tx.vin[i].scriptSig = CScript([key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        return tx

    def send(self, tx):
        return self.nodes[0].sendrawtransaction(tx.serialize().hex())

    def produce_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.nodes[0].generateposblock(self.a_wif)

    def run_test(self):
        node = self.nodes[0]
        node.generateposblock(self.a_wif)
        c_p2pk = CScript([bytes.fromhex(self.c_pub), OP_CHECKSIG])
        a_p2pk = CScript([bytes.fromhex(self.a_pub), OP_CHECKSIG])
        txid, n, value = self.free_coin(node)
        fund = self.build([(txid, n, value, None, None)],
                          [(COIN, c_p2pk), (COIN, c_p2pk), (COIN, a_p2pk)])
        fund_id = self.send(fund)
        node.generateposblock(self.a_wif)
        fund_change = value - 3 * COIN - FEE
        change = (fund_id, 3, fund_change)

        def record(controller, signer):
            return bytes.fromhex(node.getdelegationscript(controller, signer)["script"])

        self.log.info("Below the hardening height anyone may still create a record")
        _, _, x_pub = make_key()
        early = self.build([change + (None, None)], [(RECORD_VALUE, record(x_pub, self.p_pub))])
        self.send(early)
        change = (early.rehash(), 1, fund_change - RECORD_VALUE - FEE)
        node.generateposblock(self.a_wif)
        assert_equal(node.getdelegationinfo()[x_pub], self.p_pub)

        self.produce_to(HARDENING_HEIGHT - 1)  # the next block is the first hardened one

        self.log.info("From the height a record needs a coin of its controller")
        _, _, y_pub = make_key()
        unauth = self.build([change + (None, None)], [(RECORD_VALUE, record(y_pub, self.p_pub))])
        assert_raises_rpc_error(-26, "bad-delegation-unauthorized", self.send, unauth)

        self.log.info("...and may not delegate to the controller itself")
        selfdel = self.build([(fund_id, 0, COIN, self.c_key, c_p2pk)],
                             [(RECORD_VALUE, record(self.c_pub, self.c_pub))])
        assert_raises_rpc_error(-26, "bad-delegation-self", self.send, selfdel)

        self.log.info("A record spending a coin of the controller is accepted")
        c_record = record(self.c_pub, self.p_pub)
        auth = self.build([(fund_id, 0, COIN, self.c_key, c_p2pk)], [(RECORD_VALUE, c_record)])
        auth_id = self.send(auth)
        node.generateposblock(self.a_wif)
        assert node.getblockcount() >= HARDENING_HEIGHT
        assert auth_id in node.getblock(node.getbestblockhash())["tx"]
        assert_equal(node.getdelegationinfo()[self.c_pub], self.p_pub)

        self.log.info("A record spent and re-created identically in one block is refused")
        recreate = self.build([(auth_id, 0, RECORD_VALUE, self.c_key, c_record)], [(RECORD_VALUE - 2 * FEE, c_record)])
        assert_raises_rpc_error(-26, "bad-record-recreated", self.send, recreate)

        self.log.info("A payout record needs a coin of its signer")
        activation = node.getblockcount() + NOTICE + 5
        payout = bytes.fromhex(node.getpayoutscript(self.a_pub, activation, "direct", OP_TRUE.hex())["script"])
        unauth_pay = self.build([change + (None, None)], [(RECORD_VALUE, payout)])
        assert_raises_rpc_error(-26, "bad-payout-unauthorized", self.send, unauth_pay)
        # Two more coins of the signer, to authorise the records below.
        auth_pay = self.build([(fund_id, 2, COIN, self.a_key, a_p2pk)],
                              [(RECORD_VALUE, payout), (COIN // 4, a_p2pk), (COIN // 4, a_p2pk)])
        pay_id = self.send(auth_pay)
        node.generateposblock(self.a_wif)
        assert pay_id in node.getblock(node.getbestblockhash())["tx"]

        def spend_record(txid, script):
            return self.build([(txid, 0, RECORD_VALUE, self.a_key, script)], [])

        self.log.info("The payout record in force cannot be spent")
        self.produce_to(activation)
        assert_raises_rpc_error(-26, "bad-payout-in-force", self.send, spend_record(pay_id, payout))

        self.log.info("...not even once its successor is announced, until the successor binds")
        activation2 = node.getblockcount() + NOTICE + 2
        payout2 = bytes.fromhex(node.getpayoutscript(self.a_pub, activation2, "direct", a_p2pk.hex())["script"])
        pay2_id = self.send(self.build([(pay_id, 1, COIN // 4, self.a_key, a_p2pk)], [(RECORD_VALUE, payout2)]))
        node.generateposblock(self.a_wif)
        assert_raises_rpc_error(-26, "bad-payout-in-force", self.send, spend_record(pay_id, payout))

        self.log.info("A record still inside its notice may be withdrawn")
        activation3 = node.getblockcount() + NOTICE + 20
        payout3 = bytes.fromhex(node.getpayoutscript(self.a_pub, activation3, "direct", OP_TRUE.hex())["script"])
        pay3_id = self.send(self.build([(pay_id, 2, COIN // 4, self.a_key, a_p2pk)], [(RECORD_VALUE, payout3)]))
        node.generateposblock(self.a_wif)
        self.send(spend_record(pay3_id, payout3))
        node.generateposblock(self.a_wif)

        self.log.info("Once the successor binds, the superseded record is spendable and the successor is not")
        self.produce_to(activation2)
        assert_raises_rpc_error(-26, "bad-payout-in-force", self.send, spend_record(pay2_id, payout2))
        reclaim_id = self.send(spend_record(pay_id, payout))
        node.generateposblock(self.a_wif)
        assert reclaim_id in node.getblock(node.getbestblockhash())["tx"]

        self.log.info("History from below the height still validates after a restart")
        self.restart_node(0)
        assert_equal(node.getdelegationinfo()[x_pub], self.p_pub)
        assert_equal(node.verifychain(4, 0), True)


if __name__ == '__main__':
    PosHardeningTest().main()
