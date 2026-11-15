#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The signing record on a member that signs through the RPCs
(doc/sequentia/04-proof-of-stake.md, "The signing record").

A MuSig2 committee chain (-posaggcommittee) whose blocks are all made by RPC;
node0 is the parent chain ("Bitcoin"), node1 the member under test, node2 a
peer.

stop: node1 signs a block carrying a transaction T through its own node
  (musigpartialsign with the block, then submitposblock), the block connects,
  and node1 is killed at once (SIGKILL, no flush). Bitcoin replaces the block's
  anchor while node1 is down. The block index on disk may have lost the block,
  but the record names it, its height and anchor and what it spends, and that
  T is among them.
fail: a directory stands where the record goes. musigpartialsign fails with
  "cannot write the signing record", answers no partial signature, and node1
  shuts down with "Failed to write the signing record".
"""

import os
from decimal import Decimal

from test_framework.address import byte_to_base58
from test_framework.authproxy import JSONRPCException
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import get_auth_cookie, get_datadir_path, p2p_port, pos_sign_after_share_lock, rpc_port

CHAIN = "elementsregtest"


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosSigningRecordRpcTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.stakers = [make_key() for _ in range(3)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

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
            "-con_default_blinded_addresses=0", "-con_bitcoin_anchor=1", "-validateanchor=1",
            "-anchorpollinterval=1", "-anchorminconf=1", "-posescapestallmtpgap=0", "-debug=validation",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % u, "-mainchainrpcpassword=%s" % p,
            "-parentgenesisblockhash=%s" % self.nodes[0].getblockhash(0), "-txindex=1",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.add_nodes(2, [self.args(i) for i in (1, 2)], chain=[CHAIN] * 2)
        self.nodes[0].createwallet(wallet_name="w", descriptors=True)
        self.parent_mine(3)
        for i in (1, 2):
            self.start_node(i)
            self.nodes[i].createwallet(wallet_name="w", descriptors=True)
        self.connect_nodes(1, 2)

    def args(self, i):
        return self.member + ["-port=%d" % p2p_port(i), "-rpcport=%d" % rpc_port(i)]

    def parent_mine(self, n):
        p = self.nodes[0]
        return self.generatetoaddress(p, n, p.getnewaddress(), sync_fun=self.no_op)

    def certified_block(self, node):
        r = node.generateposblock(self.stakers[0][0], [w for w, _ in self.stakers[1:]])
        assert r["countersignatures"] >= 2
        return r["hash"]

    def template(self, node):
        seed = bytes.fromhex(node.getposschedule()["seed"])[::-1].hex()
        members = [{"pubkey": pub, "vrfproof": node.vrfprove(wif, seed)["proof"]} for wif, pub in self.stakers]
        return node.getposblocktemplate(self.stakers[0][0], members)

    def musig(self, node, tmpl, tag):
        msg = tmpl["signhash"]
        wif_for = {pub: wif for wif, pub in self.stakers}
        nonces = [node.musignonce("%s%d" % (tag, k), wif_for[pk], tmpl["members"], msg)["pubnonce"]
                  for k, pk in enumerate(tmpl["members"])]
        partials = [node.musigpartialsign("%s%d" % (tag, k), wif_for[pk], tmpl["members"], nonces, msg, tmpl["block"])["partialsig"]
                    for k, pk in enumerate(tmpl["members"])]
        return node.musigaggregate(tmpl["members"], nonces, partials, msg)["signature"]

    def sigkill(self, i):
        n = self.nodes[i]
        n.process.kill()
        n.process.wait()
        n.running = False
        n.process = None
        n.rpc_connected = False
        n.rpc = None

    def run_test(self):
        m = self.nodes[1]
        for _ in range(2):
            self.certified_block(m)
        self.sync_blocks(self.nodes[1:])
        self.case_stop()
        self.case_fail()

    def case_stop(self):
        m = self.nodes[1]
        # T: a transaction the member's block carries.
        g = m.getblock(m.getblockhash(0), 2)
        coin = [(tx["txid"], vo["n"], Decimal(vo["value"])) for tx in g["tx"] for vo in tx["vout"]
                if vo["scriptPubKey"]["hex"] == "51" and vo.get("value", 0) > 0][0]
        raw = m.createrawtransaction([{"txid": coin[0], "vout": coin[1]}],
                                     [{m.getnewaddress(): coin[2] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        # The block anchors on a Bitcoin block of its own, which is then replaced.
        self.parent_mine(1)
        for _ in range(8):
            if m.getblockheader(self.certified_block(m))["anchorheight"] == self.nodes[0].getblockcount():
                break
        assert m.getblockheader(m.getbestblockhash())["anchorheight"] == self.nodes[0].getblockcount()
        self.sync_blocks(self.nodes[1:])
        t = m.sendrawtransaction(raw)
        tmpl = self.template(m)
        sig = self.musig(m, tmpl, "stop")
        b = m.submitposblock(tmpl["block"], self.stakers[0][0], sig)["hash"]
        assert t in m.getblock(b)["tx"]
        anchor = m.getblockheader(b)["anchorheight"]
        self.sigkill(1)
        # Bitcoin replaces the block's anchor while the member is down.
        p = self.nodes[0]
        p.invalidateblock(p.getblockhash(anchor))
        self.parent_mine(3)
        self.start_node(1, extra_args=self.args(1))
        known = True
        try:
            m.getblockheader(b)
        except JSONRPCException:
            known = False
        entries = [e for e in m.getsigningrecord()["entries"] if e["hash"] == b]
        kinds = sorted(e["kind"] for e in entries)
        self.log.info("stop: restarted; block index knows the block: %s; record entries for it: %s" % (known, kinds))
        assert kinds == ["leader", "partial", "partial", "partial"], kinds
        e = entries[0]
        assert e["height"] == tmpl["height"] and e["anchorheight"] == anchor and e["spends"] >= 1, e
        # What it spends names T: the block's own transaction list is gone from
        # the index, so the record's spend count is checked against T's inputs.
        assert e["spends"] == len(m.decoderawtransaction(raw)["vin"])
        # Catch up with the peer before the next case signs at the next
        # height: a template made meanwhile would sit at a height whose
        # certified block is still arriving, and is refused for good.
        self.connect_nodes(1, 2)
        self.sync_blocks(self.nodes[1:])

    def case_fail(self):
        m = self.nodes[1]
        path = os.path.join(m.datadir, CHAIN, "possigned.dat")
        assert os.path.exists(path)
        os.rename(path, path + ".moved")
        os.mkdir(path)
        tmpl = self.template(m)
        msg = tmpl["signhash"]
        wif, pub = self.stakers[0]
        nonces = [m.musignonce("fail%d" % k, w, tmpl["members"], msg)["pubnonce"] for k, (w, _) in enumerate(self.stakers)]
        try:
            r = pos_sign_after_share_lock(m.musigpartialsign, "fail0", wif, tmpl["members"], nonces, msg, tmpl["block"])
            raise AssertionError("musigpartialsign answered %s with no record" % r)
        except JSONRPCException as e:
            assert "cannot write the signing record" in e.error["message"], e.error["message"]
            self.log.info("fail: musigpartialsign refused: %s" % e.error["message"])
        except (ConnectionError, OSError):
            raise AssertionError("the node went down before it answered")
        m.process.wait(timeout=60)
        m.running = False
        m.process = None
        m.rpc_connected = False
        m.rpc = None
        with open(m.debug_log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        assert "Failed to write the signing record" in text
        self.log.info("fail: node1 shut down: Failed to write the signing record")
        os.rmdir(path)
        os.rename(path + ".moved", path)
        self.start_node(1, extra_args=self.args(1))


if __name__ == '__main__':
    PosSigningRecordRpcTest().main()
