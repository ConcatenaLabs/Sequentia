#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The signing record across a Bitcoin reorganization while the member is down
(doc/sequentia/04-proof-of-stake.md, "The signing record").

node0 is the parent chain ("Bitcoin"). nodes 1-4 are a gossip committee of four
seats, one each, quorum 3; node1 (M) is the member under test. The escaping
stall is out of reach (a parent median-time gap no run reaches), so a round with
two members online never certifies.

M and node2 alone: M share-signs a block X at height H that cannot certify,
and is killed (SIGKILL) at once. While it is down:

undecided: restarted first with its parent-chain daemon unreachable, M
  cannot judge X's anchor, and signs nothing at H, a rival or not.
standing: Bitcoin keeps X's anchor. Restarted, M rebuilds its share-lock from
  the record and does not sign a rival at H before it has asked its peers for
  X's certificate (the escape: the query, then one grace round), and then it
  does sign one (liveness). Before the record M signed the next round's rival
  at once.
replaced: Bitcoin replaces X's anchor. Restarted, M finds the anchor gone, X
  holds nothing, and M signs a new block at H without asking about X: Bitcoin
  anchoring is supreme, and the record never blocks a block Bitcoin's move
  makes possible.

In both cases nodes 3 and 4 then return, a sibling of X is certified at H, and
after a block index write M's record no longer holds X: a certified sibling on
the active chain took its height.
"""

import http.client
import http.server
import json
import os
import re
import threading
import time

from feature_pos_signing_record import ShareCollector
from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import get_auth_cookie, get_datadir_path, p2p_port, rpc_port

M = 1
COMMITTEE = 4
ROUND_MS = 700 + 35 * COMMITTEE
WAIT_S = 8


class ParentGate:
    """An HTTP relay between a member and the parent chain's daemon. While shut,
    it answers no question about a block (getblockheader, getblock): the
    daemon's start-up checks pass, and no anchor can be judged."""

    def __init__(self, port, upstream_port):
        gate = self
        self.shut = False

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                try:
                    method = json.loads(body).get("method", "")
                except Exception:
                    method = ""
                if gate.shut and method in ("getblockheader", "getblock"):
                    self.close_connection = True
                    return
                c = http.client.HTTPConnection("127.0.0.1", upstream_port, timeout=30)
                c.request("POST", self.path, body, {k: v for k, v in self.headers.items() if k.lower() in ("authorization", "content-type")})
                r = c.getresponse()
                data = r.read()
                self.send_response(r.status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                c.close()

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosSigningRecordAnchorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(4)]
        self.results = {}

    def setup_network(self):
        chain = "elementsregtest"
        self.chain_name = chain
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0), "-validatepegin=0",
                            "-initialfreecoins=0", "-con_blocksubsidy=5000000000", "-anyonecanspendaremine=1",
                            "-signblockscript=51"]], chain=[chain])
        self.start_node(0)
        rpc_u, rpc_p = get_auth_cookie(get_datadir_path(self.options.tmpdir, 0), chain)
        self.common = [
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-pospubliccommittee=1",
            "-poscommitteesize=%d" % COMMITTEE, "-posexpraceheight=1", "-posslotinterval=1",
            "-con_max_block_sig_size=8000", "-con_blocksubsidy=5000000000", "-par=1", "-debug=validation",
            "-con_bitcoin_anchor=1", "-validateanchor=1", "-anchorpollinterval=1", "-anchorminconf=1",
            "-posescapestallmtpgap=1000000", "-mainchainrpctimeout=5",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.nodes[0].getblockhash(0),
        ]
        base = self.common + ["-staker=%s:1" % p for _, p in self.stakers]
        self.add_nodes(4, [base + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)] for i in range(1, 5)],
                       chain=[chain] * 4)
        self.start_node(1)
        specs = ["-staker=%s:1%s" % (p, self.nodes[1].getblsregistration(w)["spec"]) for w, p in self.stakers]
        self.full = self.common + specs
        self.stop_node(1)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_mine(3)
        for i in range(1, 5):
            self.start_node(i, extra_args=self.args_for(i))
        self.mesh()

    def args_for(self, i):
        return self.full + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i),
                            "-posproducer=1", "-posproducerkey=%s" % self.stakers[i - 1][0]]

    def parent_mine(self, n):
        p = self.nodes[0]
        return self.generatetoaddress(p, n, p.getnewaddress(), sync_fun=self.no_op)

    def mesh(self):
        up = [i for i in range(1, 5) if self.nodes[i].running]
        for a in up:
            for b in up:
                if a < b:
                    self.connect_nodes(a, b)

    def sigkill(self, i):
        n = self.nodes[i]
        n.process.kill()
        n.process.wait()
        n.running = False
        n.process = None
        n.rpc_connected = False
        n.rpc = None

    def log_since(self, i, pos):
        with open(self.nodes[i].debug_log_path, encoding="utf-8", errors="replace") as f:
            f.seek(pos)
            return f.read()

    def log_size(self, i):
        return os.path.getsize(self.nodes[i].debug_log_path)

    def record(self, key, value):
        self.results[key] = value
        self.log.info("  %s: %s", key, value)

    def run_test(self):
        m = self.nodes[M]
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= 3, timeout=120)
        self.round_case("standing")
        self.round_case("replaced")
        out = os.environ.get("CR1_RESULTS", "")
        if out:
            import json
            with open(out, "w", encoding="utf8") as f:
                json.dump(self.results, f, indent=1)
        assert m.getsigningrecord()["appends"] >= 0

    def round_case(self, case):
        m = self.nodes[M]
        # Nodes 3 and 4 and M stop cleanly: the chain halts at height H-1, which
        # M's index carries on disk. Bitcoin moves on by two blocks, so that the
        # next proposals anchor above everything H-1 rests on.
        for i in (3, 4, M):
            self.stop_node(i)
        h = self.nodes[2].getblockcount() + 1
        self.parent_mine(2)
        parent_top = self.nodes[0].getblockcount()
        pos = self.log_size(M)
        self.start_node(M, extra_args=self.args_for(M))
        self.connect_nodes(M, 2)
        x = None
        end = time.time() + 120
        while x is None and time.time() < end:
            for ln in self.log_since(M, pos).splitlines():
                mm = re.search(r"PoS gossip: signed ([0-9a-f]{64}) at height (\d+)", ln)
                if mm and int(mm.group(2)) == h:
                    x = mm.group(1)
                    break
            if x is None:
                time.sleep(0.005)
        assert x, "%s: M signed nothing at height %d" % (case, h)
        self.sigkill(M)
        self.record("%s signed" % case, "M signed %s at height %d and was killed" % (x[:16], h))
        if case == "replaced":
            # Replace every parent block from the first one mined above: X's anchor goes.
            self.nodes[0].invalidateblock(self.nodes[0].getblockhash(parent_top - 1))
            self.parent_mine(4)
        if case == "standing":
            # First a start whose parent-chain daemon does not answer: before
            # any verdict on X's anchor, M signs nothing at H, on a guess
            # either way.
            pos = self.log_size(M)
            gate = ParentGate(rpc_port(9), rpc_port(0))
            gate.shut = True
            dead = [a for a in self.args_for(M) if not a.startswith("-mainchainrpcport=")] + ["-mainchainrpcport=%d" % rpc_port(9)]
            self.start_node(M, extra_args=dead)
            self.connect_nodes(M, 2)
            col = m.add_p2p_connection(ShareCollector())
            time.sleep(WAIT_S)
            text = self.log_since(M, pos)
            waiting = "holding height %d: signed %s there before the restart; waiting for the parent chain to judge its anchor" % (h, x)
            assert waiting in text, "undecided: M did not wait for the anchor's verdict"
            mine = [b for b, pk in col.shares if pk == self.stakers[M - 1][1]]
            assert not mine, "undecided: M signed %s with its anchor unjudged" % mine
            assert "PoS gossip: signed" not in text
            self.record("undecided", "parent daemon unreachable: M held height %d for %d s and signed nothing" % (h, WAIT_S))
            self.stop_node(M)
            gate.close()
        pos = self.log_size(M)
        t_start = time.time()
        self.start_node(M, extra_args=self.args_for(M))
        # M may also have led X (a proposal entry); the share is what holds the height.
        entries = [e for e in m.getsigningrecord()["entries"] if e["hash"] == x and e["kind"] == "share"]
        assert entries and entries[0]["kind"] == "share" and entries[0]["height"] == h, entries
        self.record("%s record" % case, "after the restart the record holds X at %d, anchor %d (parent top was %d)" %
                    (h, entries[0]["anchorheight"], parent_top))
        assert entries[0]["anchorheight"] >= parent_top - 1
        self.connect_nodes(M, 2)
        signed_rival = None
        end = time.time() + 60
        while signed_rival is None and time.time() < end:
            for ln in self.log_since(M, pos).splitlines():
                mm = re.search(r"PoS gossip: signed ([0-9a-f]{64}) at height (\d+)", ln)
                if mm and int(mm.group(2)) == h and mm.group(1) != x:
                    signed_rival = mm.group(1)
                    break
            if signed_rival is None:
                time.sleep(0.05)
        assert signed_rival, "%s: the restarted M never signed at height %d" % (case, h)
        text = self.log_since(M, pos)
        before = text.split("PoS gossip: signed %s" % signed_rival, 1)[0]
        restored = "share-lock restored from the signing record: signed %s at height %d" % (x, h)
        queried = "share-lock at %s" % x
        replaced = "signed %s at height %d before the restart; Bitcoin replaced its anchor" % (x, h)
        if case == "standing":
            assert restored in before, "standing: M did not rebuild its share-lock from the record"
            assert queried in before, "standing: M signed a rival of X before asking for X's certificate"
            self.record("standing", "M rebuilt the share-lock on X, asked for X's certificate, then signed %s at %d, %.1f s after its start" %
                        (signed_rival[:16], h, time.time() - t_start))
        else:
            assert replaced in text, "replaced: M did not judge X's anchor replaced"
            assert restored not in text and queried not in before, "replaced: X held its height though Bitcoin replaced its anchor"
            self.record("replaced", "X's anchor gone: M signed %s at %d without asking about X" % (signed_rival[:16], h))
        # Everyone back: a sibling of X is certified at H.
        for i in (3, 4):
            self.start_node(i, extra_args=self.args_for(i))
        self.mesh()
        self.wait_until(lambda: min(self.nodes[i].getblockcount() for i in range(1, 5)) >= h + 1, timeout=180)
        self.sync_blocks(self.nodes[1:], timeout=60)
        at_h = m.getblockhash(h)
        assert at_h != x and m.getblockheader(at_h)["poscertified"]
        m.gettxoutsetinfo()  # a block index write
        left = [e for e in m.getsigningrecord()["entries"] if e["hash"] == x]
        assert not left, "%s: X stays in the record after a certified sibling took its height" % case
        rec = m.getsigningrecord()
        self.record("%s drop" % case, "after the index write: X dropped; %d entr(ies), %d bytes on disk; appends %d, last %d us, slowest %d us, mean %.0f us" %
                    (len(rec["entries"]), rec["file_bytes"], rec["appends"], rec["last_append_us"], rec["max_append_us"],
                     rec["total_append_us"] / max(1, rec["appends"])))


if __name__ == '__main__':
    PosSigningRecordAnchorTest().main()
