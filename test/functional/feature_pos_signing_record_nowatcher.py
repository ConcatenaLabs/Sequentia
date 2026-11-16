#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""On a node that does not watch Bitcoin (-validateanchor=0), invalidating a
certified block releases the member after one grace round, as on a node that
does: whatever kind of entry holds the height, before and after a restart,
with the block and as a bare hash (doc/sequentia/04-proof-of-stake.md, "The
signing record").

The MuSig2 committee chain of feature_pos_signing_record_rpc.py: node0 is the
parent chain. Every member key is held by the node that signs, by RPC, with no
producer thread. Each case needs its X at height 1, so that no certified block
stands at or above the tip once X is invalidated (a bare hash is refused for
good while one does), and so has a chain of its own.

certonly: node1 (M, no watcher) learns X, certified at height 1, from node2
  (the peer, which made it), and holds no signature of its own on X. M
  invalidates X: a partial signature over the bare hash of a rival is refused,
  naming X, and made once one round has passed; X is reconsidered and
  invalidated again, and a rival with its block is likewise refused for one
  round, then made. M restarts with X standing, invalidates it again, and the
  same holds.
signed: node3 (M, no watcher) signs and certifies X at height 1 itself. The
  same, in this run and after a clean restart, by which the record has dropped
  its now redundant entries for X (the block index carries X's certificate),
  so that X is held as a certified block restored from the disk.
--testnet-settings runs at the testnet's round length and window and its
anchor watcher's poll (-posroundms=9450 -poswindowms=6750
-anchorpollinterval=15).
"""

import time

from feature_pos_signing_record_paths import block_hash
from feature_pos_signing_record_rpc import CHAIN, PosSigningRecordRpcTest, make_key
from feature_pos_signing_record_paths import ROUND_S
from feature_pos_signing_record_state import TESTNET_SETTINGS, released_after_one_round
from test_framework.util import assert_equal, get_auth_cookie, get_datadir_path, p2p_port, rpc_port


class PosSigningRecordNoWatcherTest(PosSigningRecordRpcTest):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.stakers = [make_key() for _ in range(3)]

    def add_options(self, parser):
        parser.add_argument("--only", choices=("certonly", "certonly-restart", "signed", "signed-restart"),
                            help="run one case only (-restart: only its part after the restart)")
        parser.add_argument("--testnet-settings", dest="testnet_settings", default=False, action="store_true",
                            help="the testnet's round length, window and anchor watcher poll")

    def setup_network(self):
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-signblockscript=51"]], chain=[CHAIN])
        self.start_node(0)
        u, p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), CHAIN)
        self.member = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posaggcommittee=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-con_blocksubsidy=0", "-initialfreecoins=1000000000000", "-con_connect_genesis_outputs=1",
            "-acceptnonstdtxn=1", "-par=1", "-fallbackfee=0.0001", "-blindedaddresses=0",
            "-con_default_blinded_addresses=0", "-con_bitcoin_anchor=1",
            "-anchorpollinterval=1", "-anchorminconf=1", "-posescapestallmtpgap=0", "-debug=validation",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % u, "-mainchainrpcpassword=%s" % p,
            "-parentgenesisblockhash=%s" % self.nodes[0].getblockhash(0), "-txindex=1",
            "-posfinalitydelayms=60000",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.add_nodes(3, [self.args(i) for i in (1, 2, 3)], chain=[CHAIN] * 3)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_mine(3)
        for i in (1, 2, 3):
            self.start_node(i)

    def args(self, i):
        # The peer (node2) watches Bitcoin; the members under test do not.
        watch = "-validateanchor=1" if i == 2 else "-validateanchor=0"
        a = self.member + [watch, "-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)]
        if self.options.testnet_settings:
            a = [x for x in a if not x.startswith("-anchorpollinterval")] + TESTNET_SETTINGS
        return a

    @property
    def round_s(self):
        return 9.45 + 0.3 if self.options.testnet_settings else ROUND_S

    def run_test(self):
        for i in (1, 3):
            assert_equal(self.nodes[i].getanchorstatus()["validateanchor"], False)
        only = self.options.only
        if only in (None, "certonly", "certonly-restart"):
            self.case_certonly(this_run=only != "certonly-restart")
        if only in (None, "signed", "signed-restart"):
            self.case_signed(this_run=only != "signed-restart")

    # --- helpers ---

    def generate(self, node):
        return node.generateposblock(self.stakers[0][0], [w for w, _ in self.stakers[1:]])

    def held(self, node, x):
        b = [b for b in node.getsigningrecord()["held"] if b["hash"] == x]
        return b[0] if b else None

    def entries(self, node, x):
        return sorted(e["kind"] for e in node.getsigningrecord()["entries"] if e["hash"] == x)

    def bare_partial(self, m, tmpl, tag):
        wif_for = {pub: wif for wif, pub in self.stakers}
        pk = tmpl["members"][0]
        nonces = [m.musignonce("%s%d" % (tag, k), wif_for[p], tmpl["members"], tmpl["signhash"])["pubnonce"]
                  for k, p in enumerate(tmpl["members"])]
        return m.musigpartialsign("%s0" % tag, wif_for[pk], tmpl["members"], nonces, tmpl["signhash"])

    def rivals(self, m, n):
        """n templates at height 1, each of another second, made while M's tip is the genesis block."""
        out = []
        while len(out) < n:
            if out:
                time.sleep(1.1)
            t = self.template(m)
            assert_equal(t["height"], 1)
            if block_hash(t) not in [block_hash(o) for o in out]:
                out.append(t)
        return out

    def released(self, case, m, x, rival, tag):
        """X invalid here: a bare hash, then (X revived and invalidated again)
        a rival with its block, each refused naming X for one round, then made."""
        assert_equal(m.getblockcount(), 0)
        h = self.held(m, x)
        self.log.info("%s: X invalidated; the check holds it: %s" % (case, h))
        assert h and h["certified"] and h["invalidated"], h
        r = released_after_one_round(self.log, case, "a partial signature over the bare hash of a rival",
                                     lambda k: self.bare_partial(m, rival, "%sb%d" % (tag, k)), x, self.round_s)
        assert "partialsig" in r
        # Revived and invalidated again, with no signing check between: the
        # new failure owes a round of its own.
        m.reconsiderblock(x)
        assert_equal(m.getbestblockhash(), x)
        m.invalidateblock(x)
        assert_equal(m.getblockcount(), 0)
        r = released_after_one_round(self.log, case, "a rival with its block (musigpartialsign), X revived and invalidated again",
                                     lambda k: self.musig(m, rival, "%sm%d" % (tag, k)), x, self.round_s)
        assert r
        m.reconsiderblock(x)
        assert_equal(m.getbestblockhash(), x)

    def restart(self, i):
        self.stop_node(i)
        self.start_node(i, extra_args=self.args(i))
        assert_equal(self.nodes[i].getanchorstatus()["validateanchor"], False)

    # --- cases ---

    def case_certonly(self, this_run):
        m, peer = self.nodes[1], self.nodes[2]
        r1, r2 = self.rivals(m, 2)
        time.sleep(1.1)
        x = self.generate(peer)["hash"]
        assert x not in (block_hash(r1), block_hash(r2))
        self.connect_nodes(1, 2)
        self.sync_blocks([m, peer])
        # Alone, so that the peer does not offer X again while it is invalid here.
        self.disconnect_nodes(1, 2)
        assert_equal(m.getbestblockhash(), x)
        assert_equal(self.entries(m, x), [])
        h = self.held(m, x)
        self.log.info("certonly: M learned X=%s at 1 from the peer, with no signature of its own: %s" % (x[:16], h))
        assert h and h["certified"] and not h["signed"], h
        if this_run:
            m.invalidateblock(x)
            self.released("certonly", m, x, r1, "c1")
        self.restart(1)
        m = self.nodes[1]
        h = self.held(m, x)
        self.log.info("certonly: restarted; X restored from the block index: %s" % h)
        assert h and h["certified"] and not h["signed"] and h["restored"], h
        m.invalidateblock(x)
        self.released("certonly after a restart", m, x, r2, "c2")

    def case_signed(self, this_run):
        m = self.nodes[3]
        r1, r2 = self.rivals(m, 2)
        time.sleep(1.1)
        x = self.generate(m)["hash"]
        assert x not in (block_hash(r1), block_hash(r2))
        assert_equal(m.getblockheader(x)["height"], 1)
        kinds = self.entries(m, x)
        self.log.info("signed: M signed and certified X=%s at 1: entries %s" % (x[:16], kinds))
        assert kinds, kinds
        if this_run:
            m.invalidateblock(x)
            self.released("signed", m, x, r1, "s1")
        self.restart(3)
        m = self.nodes[3]
        kinds = self.entries(m, x)
        h = self.held(m, x)
        self.log.info("signed: restarted; entries for X %s (the index carries its certificate); held %s" % (kinds, h))
        assert_equal(kinds, [])
        assert h and h["certified"] and not h["signed"] and h["restored"], h
        m.invalidateblock(x)
        self.released("signed after a restart", m, x, r2, "s2")


if __name__ == "__main__":
    PosSigningRecordNoWatcherTest().main()
