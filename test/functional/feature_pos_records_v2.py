#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Second-generation stake records (audit M4), on both sides of -posrecordsv2height.

The bare stake record scripts (staking, unbonding, delegation, payout) were
spent with the legacy signature hash, which commits to no amount, so a signer
shown only the transaction could be lied to about the fee; and their scriptSig
could be re-encoded by anyone relaying it, changing the txid. From the height
such a spend signs the segwit-v0 hash, which commits to the amount spent, and
its scriptSig is canonical: one minimally pushed, low-S signature.

  * below the height only the legacy signature is valid;
  * a legacy-signed spend waiting in the mempool is evicted at the boundary;
  * from the height only the new signature is valid, and none of its
    re-encodings (an extra push, a non-minimal push, the high-S twin) is.
"""

from test_framework.address import byte_to_base58
from test_framework.key import ECKey, SECP256K1_ORDER
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import (
    CScript, LegacySignatureHash, OP_0, OP_CHECKSIG, OP_PUSHDATA1, PosRecordSignatureHash, SIGHASH_ALL,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

COIN = 100_000_000
FEE = 100_000
RECORD_VALUE = 1_000_000
V2_HEIGHT = 12
OP_TRUE = CScript([0x51])


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return k, byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


def der_s(sig):
    """The S value of a DER signature (without the sighash byte)."""
    rlen = sig[3]
    slen = sig[5 + rlen]
    return int.from_bytes(sig[6 + rlen:6 + rlen + slen], 'big')


def high_s_twin(sig):
    """The same signature with S replaced by n - S: valid for ECDSA, refused
    by the low-S rule."""
    rlen = sig[3]
    r = sig[4:4 + rlen]
    s = SECP256K1_ORDER - der_s(sig)
    sb = s.to_bytes((s.bit_length() + 8) // 8, 'big')
    body = bytes([0x02, len(r)]) + r + bytes([0x02, len(sb)]) + sb
    return bytes([0x30, len(body)]) + body


class PosRecordsV2Test(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.a_key, self.a_wif, self.a_pub = make_key()   # producer
        self.c_key, self.c_wif, self.c_pub = make_key()   # a controller
        self.p_key, self.p_wif, self.p_pub = make_key()   # a pool
        self.extra_args = [[
            "-con_pos=1", "-posvrf=1", "-posunbonding=5", "-posslotinterval=1",
            "-posrecordsv2height=%d" % V2_HEIGHT,
            "-signblockscript=51", "-initialfreecoins=1000000000000", "-anyonecanspendaremine=0",
            "-con_blocksubsidy=0", "-con_connect_genesis_outputs=1", "-acceptnonstdtxn=1",
            "-staker=%s:%d" % (self.a_pub, COIN), "-validatepegin=0", "-persistmempool=0",
        ]] * 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def free_coin(self, node):
        genesis = node.getblock(node.getblockhash(0), 2)
        for tx in genesis['tx']:
            for vout in tx['vout']:
                if vout['scriptPubKey']['hex'] == '51' and vout.get('value', 0) > 0:
                    return tx['txid'], vout['n'], int(vout['value'] * COIN)
        raise AssertionError("no OP_TRUE genesis output")

    def unsigned(self, inputs, outs):
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n), nSequence=0xfffffffe) for txid, n, _ in inputs]
        rest = sum(v for _, _, v in inputs) - sum(v for v, _ in outs) - FEE
        tx.vout = [CTxOut(v, s) for v, s in outs] + [CTxOut(rest, OP_TRUE), CTxOut(FEE)]
        return tx

    def record_spend(self, record_id, record, v2, low_s=True):
        """A spend of the delegation record, signed by its controller, and the
        raw signature (with sighash byte)."""
        tx = self.unsigned([(record_id, 0, RECORD_VALUE)], [])
        sighash, err = PosRecordSignatureHash(CScript(record), tx, 0, SIGHASH_ALL, RECORD_VALUE, v2=v2)
        assert err is None
        while True:
            sig = self.c_key.sign_ecdsa(sighash, low_s=low_s)
            if low_s or der_s(sig) > SECP256K1_ORDER // 2:
                break
        sig += bytes([SIGHASH_ALL])
        tx.vin[0].scriptSig = CScript([sig])
        tx.rehash()
        return tx, sig

    def send(self, node, tx):
        return node.sendrawtransaction(tx.serialize().hex())

    def produce(self, node, n=1):
        for _ in range(n):
            node.generateposblock(self.a_wif)

    def make_record(self, node, coin):
        """A delegation record of the controller, created by spending one of its
        P2PK coins (the hardening rule)."""
        record = bytes.fromhex(node.getdelegationscript(self.c_pub, self.p_pub)["script"])
        c_p2pk = CScript([bytes.fromhex(self.c_pub), OP_CHECKSIG])
        txid, n, value = coin
        tx = self.unsigned([(txid, n, value)], [(RECORD_VALUE, record)])
        sighash, err = LegacySignatureHash(c_p2pk, tx, 0, SIGHASH_ALL)
        assert err is None
        tx.vin[0].scriptSig = CScript([self.c_key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])])
        return self.send(node, tx), record

    def run_test(self):
        node, other = self.nodes
        self.produce(node)
        c_p2pk = CScript([bytes.fromhex(self.c_pub), OP_CHECKSIG])
        txid, n, value = self.free_coin(node)
        wallet_spk = CScript(bytes.fromhex(node.getaddressinfo(node.getnewaddress())["scriptPubKey"]))
        fund = self.unsigned([(txid, n, value)], [(COIN, c_p2pk), (COIN, c_p2pk), (10 * COIN, wallet_spk)])
        fund_id = self.send(node, fund)
        self.produce(node)

        rec1_id, record = self.make_record(node, (fund_id, 0, COIN))
        self.produce(node)

        self.log.info("Below the height a record spend signing the new hash is refused...")
        new_sig_early, _ = self.record_spend(rec1_id, record, v2=True)
        assert_raises_rpc_error(-26, "script-verify-flag-failed", self.send, node, new_sig_early)

        self.log.info("...and the legacy one is valid")
        legacy, _ = self.record_spend(rec1_id, record, v2=False)
        legacy_id = self.send(node, legacy)
        self.produce(node)
        assert legacy_id in node.getblock(node.getbestblockhash())["tx"]

        self.log.info("A legacy-signed spend admitted for the block before the height is evicted at it")
        rec2_id, _ = self.make_record(node, (fund_id, 1, COIN))
        while node.getblockcount() < V2_HEIGHT - 3:
            self.produce(node)
        self.sync_all()
        assert_equal(node.getblockcount(), V2_HEIGHT - 3)
        self.disconnect_nodes(0, 1)
        self.produce(node)  # confirms the record, tip V2_HEIGHT - 2
        waiting, _ = self.record_spend(rec2_id, record, v2=False)
        waiting_id = self.send(node, waiting)  # judged for block V2_HEIGHT - 1
        assert waiting_id in node.getrawmempool()
        # The block before the height arrives without it, from a producer that
        # never saw it.
        other.submitblock(node.getblock(node.getbestblockhash(), 0))
        self.produce(other)
        assert_equal(other.getblockcount(), V2_HEIGHT - 1)
        node.submitblock(other.getblock(other.getbestblockhash(), 0))
        assert_equal(node.getblockcount(), V2_HEIGHT - 1)
        assert waiting_id not in node.getrawmempool()
        self.connect_nodes(0, 1)

        self.log.info("From the height the legacy signature is refused")
        assert_raises_rpc_error(-26, "script-verify-flag-failed", self.send, node, waiting)

        self.log.info("...and so is every re-encoding of the new one")
        good, sig = self.record_spend(rec2_id, record, v2=True)
        extra_push = CTransaction(good)
        extra_push.vin[0].scriptSig = CScript([OP_0, sig])
        assert_raises_rpc_error(-26, "script-verify-flag-failed", self.send, node, extra_push)
        non_minimal = CTransaction(good)
        non_minimal.vin[0].scriptSig = CScript(bytes([OP_PUSHDATA1, len(sig)]) + sig)
        assert_raises_rpc_error(-26, "script-verify-flag-failed", self.send, node, non_minimal)
        high_s = CTransaction(good)
        high_s.vin[0].scriptSig = CScript([high_s_twin(sig[:-1]) + bytes([SIGHASH_ALL])])
        assert_raises_rpc_error(-26, "script-verify-flag-failed", self.send, node, high_s)

        self.log.info("The canonical spend signing the amount is valid")
        good_id = self.send(node, good)
        self.produce(node)
        assert good_id in node.getblock(node.getbestblockhash())["tx"]
        self.sync_all()

        self.log.info("The wallet signs record spends for the new rules (withdrawstake)")
        w_key, w_wif, w_pub = make_key()
        node.importprivkey(w_wif, "", False)
        node.registerstake(w_pub, 2)
        self.produce(node, 6)
        withdrawn = node.withdrawstake(w_pub)
        self.produce(node)
        assert withdrawn["txid"] in node.getblock(node.getbestblockhash())["tx"]

        self.log.info("History from both sides validates after a restart")
        self.restart_node(0)
        assert_equal(node.verifychain(4, 0), True)


if __name__ == '__main__':
    PosRecordsV2Test().main()
