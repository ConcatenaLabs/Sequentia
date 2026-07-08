#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license.
"""OpenAMP confidential (opt-in) assets, end to end against a real openampd.

Requires the openampd binary:
    cd ~/openamp && go build ./openampd/cmd/openampd
    OPENAMPD=~/openamp/openampd/openampd test/functional/feature_openamp_confidential.py

Proves: a confidential restricted asset is issued into a blinded enclave
output (amounts and asset tags hidden on-chain), the policy server reports the
correct unblinded balances (via its watch wallet), and a confidential transfer
settles with the recipient's output blinded on-chain and balances conserved.
"""

import http.client
import json
import os
import subprocess
import time
from urllib.parse import urlparse

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr


class OpenAmpConfidentialTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-initialfreecoins=2100000000000000",
            "-anyonecanspendaremine=1",
            "-blindedaddresses=1",   # confidential wallet addresses for blinding
            "-validatepegin=0",
            "-con_parent_chain_signblockscript=51",
            "-con_any_asset_fees=1",
            "-maxtxfee=100.0",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bdb()
        if not os.environ.get("OPENAMPD"):
            raise SkipTest("OPENAMPD env var not set (path to openampd binary)")

    def setup_network(self, split=False):
        self.setup_nodes()

    def api(self, method, path, body=None, token=None, expect=200):
        conn = http.client.HTTPConnection("127.0.0.1", self.amp_port, timeout=60)
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        conn.request(method, path, json.dumps(body) if body is not None else None, headers)
        resp = conn.getresponse()
        data = resp.read().decode()
        conn.close()
        if resp.status != expect:
            raise AssertionError("%s %s -> %d (want %d): %s" % (method, path, resp.status, expect, data))
        return json.loads(data) if data else None

    def wait_height(self, h):
        for _ in range(100):
            r = self.api("GET", "/v1/issuer/holders?asset=" + self.asset, token="testtoken")
            if r["height"] >= h:
                return
            time.sleep(0.1)
        raise AssertionError("daemon never reached height %d" % h)

    def sign_and_complete(self, transfer, key):
        sigs = {str(e["input"]): sign_schnorr(key, bytes.fromhex(e["sighash"])).hex() for e in transfer["to_sign"]}
        return self.api("POST", "/v1/transfers/%s/complete" % transfer["id"], {"sigs": sigs})

    def is_blinded(self, txid, vout):
        v = self.nodes[0].getrawtransaction(txid, True)["vout"][vout]
        return "valuecommitment" in v and "assetcommitment" in v

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(node.getnewaddress(), 1000)  # a clean spendable utxo
        self.generate(node, 1)
        self.mineaddr = node.getnewaddress()

        url = urlparse(node.url)
        self.amp_port = 18922 + self.options.port_seed % 1000
        datadir = os.path.join(self.options.tmpdir, "openampd")
        proc = subprocess.Popen([
            os.environ["OPENAMPD"],
            "-listen", "127.0.0.1:%d" % self.amp_port,
            "-datadir", datadir,
            "-rpc", "http://%s:%d" % (url.hostname, url.port),
            "-rpcauth", "%s:%s" % (url.username, url.password),
            "-rpcwallet", self.default_wallet_name,  # scope default wallet (watch wallet loads alongside)
            "-issuertoken", "testtoken", "-demoissuer",
            "-feesats", "20000", "-follow", "200ms",
        ], stdout=open(os.path.join(self.options.tmpdir, "openampd-conf.log"), "w"), stderr=subprocess.STDOUT)
        try:
            self.run_with_daemon(node)
        finally:
            proc.terminate()
            proc.wait(timeout=10)

    def run_with_daemon(self, node):
        for _ in range(100):
            try:
                self.api("GET", "/v1/assets"); break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)

        self.log.info("register participants")
        keys, aids = {}, {}
        for who in ("issuer", "alice", "bob"):
            keys[who] = generate_privkey()
            pub = compute_xonly_pubkey(keys[who])[0].hex()
            aids[who] = self.api("POST", "/v1/users", {"pubkeys": [pub]})["aid"]

        self.log.info("issue a CONFIDENTIAL asset into Alice's enclave")
        res = self.api("POST", "/v1/issuer/assets", {
            "name": "OpenAMP Confidential Bond", "ticker": "CBND", "precision": 8,
            "atoms": 100 * 100000000, "holder_aid": aids["alice"], "issuer_aid": aids["issuer"],
            "burn_allowed": True, "confidential": True,
            "rules": {"fee_convert_atoms": 100},
        }, token="testtoken")
        self.asset = res["asset"]
        issue_txid = res["txid"]
        assert_equal(res["contract"]["openamp"]["confidential"], True)
        assert issue_txid in node.getrawmempool()
        self.generatetoaddress(node, 1, self.mineaddr)

        self.log.info("enclave output is BLINDED on-chain")
        assert self.is_blinded(issue_txid, 0), "issuance enclave output is not confidential on-chain"

        self.wait_height(node.getblockcount())
        bal = self.api("GET", "/v1/users/%s/balance?asset=%s" % (aids["alice"], self.asset))
        self.log.info("server reports Alice's unblinded balance: %d atoms" % bal["atoms"])
        assert_equal(bal["atoms"], 100 * 100000000)

        self.log.info("confidential transfer Alice -> Bob (40 CBND, fee via conversion)")
        t = self.api("POST", "/v1/transfers", {
            "asset": self.asset, "sender_aid": aids["alice"], "recipient_aid": aids["bob"],
            "atoms": 40 * 100000000, "fee_mode": "convert",
        })
        r = self.sign_and_complete(t, keys["alice"])
        xfer_txid = r["txid"]
        assert xfer_txid in node.getrawmempool()
        self.generatetoaddress(node, 1, self.mineaddr)
        self.wait_height(node.getblockcount())

        self.log.info("transfer recipient output is BLINDED on-chain")
        assert self.is_blinded(xfer_txid, 0), "transfer recipient output is not confidential on-chain"

        self.log.info("balances conserved and correct (server-unblinded)")
        report = self.api("GET", "/v1/issuer/holders?asset=" + self.asset, token="testtoken")
        assert_equal(report["holders"][aids["bob"]], 40 * 100000000)
        assert_equal(report["holders"][aids["alice"]], 60 * 100000000 - 100)
        assert_equal(report["holders"][aids["issuer"]], 100)
        assert_equal(report["total_atoms"], 100 * 100000000)

        # A third party (no blinding key) cannot read the amounts: the chain
        # shows only commitments.
        assert self.is_blinded(issue_txid, 0) and self.is_blinded(xfer_txid, 0)
        self.log.info("CONFIDENTIAL e2e complete: amounts/assets hidden on-chain, "
                      "server sees and reports exact balances")


if __name__ == "__main__":
    OpenAmpConfidentialTest().main()
