#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The signing check on every RPC signing path (doc/sequentia/04-proof-of-stake.md,
"The signing record").

The MuSig2 committee chain of feature_pos_signing_record_rpc.py: node0 is the
parent chain, node1 (M) holds every member's key and makes every block by RPC,
with no producer thread. Each case gets M to sign a block X at a height and
then asks it, by another path, to sign a rival there, in the same run or after
M was killed (SIGKILL) and started again:

musig: M's partial signatures on X (musigpartialsign with the block) bind its
  height: a partial signature on a rival is refused, naming X, and M asks its
  peers for X's certificate; the same call succeeds once one round has passed
  with no certificate.
submit: after the restart, submitposblock refuses the leader's signature on
  the rival until a round has passed, from the record alone; a partial
  signature over a bare hash that M does not know is refused until it is given
  the block.
generate: after the restart, generateposblock refuses a rival of a block M
  signed partially before it, then makes one a round later.
The wallet's signblock is in feature_pos_signing_record_signblock.py.
"""

import time

from feature_pos_signing_record_rpc import PosSigningRecordRpcTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal

ROUND_S = (700 + 35 * 3) / 1000.0 + 0.3


def block_hash(tmpl):
    return bytes.fromhex(tmpl["signhash"])[::-1].hex()


class PosSigningRecordPathsTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        super().set_test_params()

    def add_options(self, parser):
        parser.add_argument("--only", choices=("musig", "submit", "generate"), help="run one case only")

    def run_test(self):
        m = self.nodes[1]
        self.certified_block(m)
        only = self.options.only
        if only in (None, "musig", "submit"):
            self.case_musig_and_submit(only)
        if only in (None, "generate"):
            self.case_generate()

    def refused(self, what, call, bound):
        try:
            r = call()
        except JSONRPCException as e:
            msg = e.error["message"]
            assert bound is None or bound in msg, "%s: refused for another reason: %s" % (what, msg)
            self.log.info("%s: refused: %s" % (what, msg))
            return msg
        raise AssertionError("%s: signed (%s) while this member's signature on %s binds the height" % (what, str(r)[:60], bound and bound[:16]))

    def log_text(self):
        with open(self.nodes[1].debug_log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def rival_templates(self, node):
        first = self.template(node)
        second = self.template(node)
        while second["signhash"] == first["signhash"]:
            time.sleep(1.1)
            second = self.template(node)
        assert_equal(first["height"], second["height"])
        return first, second

    def restart(self):
        self.sigkill(1)
        self.start_node(1, extra_args=self.args(1))
        self.connect_nodes(1, 2)

    def case_musig_and_submit(self, only):
        m = self.nodes[1]
        first, second = self.rival_templates(m)
        x = block_hash(first)
        h = first["height"]
        self.musig(m, first, "x")
        self.log.info("M signed X=%s at %d (three partial signatures, with the block)" % (x[:16], h))
        if only != "submit":
            self.refused("musig: a rival's partial signature in the same run", lambda: self.musig(m, second, "y1"), x)
            assert "share-lock at %s" % x in self.log_text(), "musig: M did not ask for X's certificate"
        time.sleep(ROUND_S)
        second_sig = self.musig(m, second, "y2")
        self.log.info("musig: one round later the same call signed the rival %s" % block_hash(second)[:16])

        self.restart()
        if only != "musig":
            # The leader's signature on the rival: X binds from the record.
            self.refused("submit: the leader's signature on the rival after the restart",
                         lambda: m.submitposblock(second["block"], self.stakers[0][0], second_sig), x)
            # A bare hash M does not know: refused until the block is given.
            third = self.template(m)
            while third["signhash"] in (first["signhash"], second["signhash"]):
                time.sleep(1.1)
                third = self.template(m)
            wif_for = {pub: wif for wif, pub in self.stakers}
            pk = third["members"][0]
            nonces = [m.musignonce("bare%d" % k, wif_for[p], third["members"], third["signhash"])["pubnonce"]
                      for k, p in enumerate(third["members"])]
            msg = self.refused("submit: a partial signature over a bare hash after the restart",
                               lambda: m.musigpartialsign("bare0", wif_for[pk], third["members"], nonces, third["signhash"]), None)
            assert "give the block" in msg, msg
            time.sleep(ROUND_S)
        r = m.submitposblock(second["block"], self.stakers[0][0], second_sig)
        assert_equal(r["height"], h)
        self.log.info("submit: one round later the rival %s connected at %d" % (r["hash"][:16], h))

    def case_generate(self):
        m = self.nodes[1]
        t = self.template(m)
        x = block_hash(t)
        self.musig(m, t, "g")
        self.log.info("generate: M signed X=%s at %d partially, then was killed" % (x[:16], t["height"]))
        self.restart()
        # A template of another second: in X's second, generateposblock
        # rebuilds X itself, which is the same block again, not a rival.
        time.sleep(1.1)
        self.refused("generate: generateposblock after the restart",
                     lambda: m.generateposblock(self.stakers[0][0], [w for w, _ in self.stakers[1:]]), x)
        time.sleep(ROUND_S)
        r = m.generateposblock(self.stakers[0][0], [w for w, _ in self.stakers[1:]])
        assert_equal(m.getblockheader(r["hash"])["height"], t["height"])
        self.log.info("generate: one round later generateposblock made %s at %d" % (r["hash"][:16], t["height"]))

if __name__ == "__main__":
    PosSigningRecordPathsTest().main()
