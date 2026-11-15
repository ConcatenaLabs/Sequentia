#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The wallet's signblock signs no proof-of-stake block
(doc/sequentia/04-proof-of-stake.md, "The signing record").

A leader-only chain (a committee of one, no anchoring), where a legacy wallet
holding the leader's key could otherwise sign a block whole: signblock is
refused, saying where proof-of-stake blocks are signed, and the signing record
is unchanged.
"""

from io import BytesIO

from feature_pos_signing_record_rpc import PosSigningRecordRpcTest
from test_framework.authproxy import JSONRPCException
from test_framework.messages import CBlock
from test_framework.util import assert_equal


class PosSigningRecordSignblockTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        super().set_test_params()

    def args(self, i):
        return [a.replace("-posaggcommittee=1", "-posaggcommittee=0")
                 .replace("-con_bitcoin_anchor=1", "-con_bitcoin_anchor=0")
                 .replace("-validateanchor=1", "-validateanchor=0")
                 .replace("-poscommitteesize=3", "-poscommitteesize=1") for a in super().args(i)]

    def run_test(self):
        m = self.nodes[1]
        m.createwallet(wallet_name="legacy", descriptors=False)
        wallet = m.get_wallet_rpc("legacy")
        for wif, _ in self.stakers:
            wallet.importprivkey(wif)
        made = m.generateposblock(self.stakers[0][0], [])
        raw = m.getblock(made["hash"], 0)
        m.invalidateblock(made["hash"])
        block = CBlock()
        block.deserialize(BytesIO(bytes.fromhex(raw)))
        block.nTime += 1
        block.proof.solution = b""
        block.rehash()
        before = len(m.getsigningrecord()["entries"])
        try:
            sigs = wallet.signblock(block.serialize().hex())
        except JSONRPCException as e:
            assert "does not sign proof-of-stake blocks" in e.error["message"], e.error["message"]
            self.log.info("signblock refused: %s" % e.error["message"])
        else:
            raise AssertionError("signblock returned %d signature(s) on a proof-of-stake block" % len(sigs))
        assert_equal(len(m.getsigningrecord()["entries"]), before)


if __name__ == "__main__":
    PosSigningRecordSignblockTest().main()
