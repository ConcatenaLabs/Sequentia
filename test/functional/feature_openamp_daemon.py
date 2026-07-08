#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""OpenAMP M1/M3 e2e: drives a real openampd against a real node.

Requires the openampd binary; skipped otherwise:

    cd ~/openamp && go build ./openampd/cmd/openampd
    OPENAMPD=~/openamp/openampd test/functional/feature_openamp_daemon.py

Covers: registration, hosted issuance into the enclave (binding re-verified
in python), hosted transfers with fee conversion, freeze refusal, raw co-sign
with a self-paid fee, velocity limits, holder caps, clawback, the ownership
report, and transparency-log integrity with on-chain anchoring.
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
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    CTxOutAsset,
    CTxOutValue,
    sha256,
    tx_from_hex,
)

from feature_openamp_m0 import derive_issuance_ids  # reuse the M0 reference implementation


class OpenAmpDaemonTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-initialfreecoins=2100000000000000",
            "-anyonecanspendaremine=1",
            "-blindedaddresses=0",
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

    # -- HTTP helpers --------------------------------------------------------

    def api(self, method, path, body=None, token=None, expect=200):
        conn = http.client.HTTPConnection("127.0.0.1", self.amp_port, timeout=30)
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

    def wait_daemon_height(self, height):
        for _ in range(100):
            got = self.api("GET", "/v1/issuer/holders?asset=" + self.asset, token="testtoken")["height"]
            if got >= height:
                return
            time.sleep(0.1)
        raise AssertionError("daemon never reached height %d" % height)

    def sign_and_complete(self, transfer, key):
        sigs = {}
        for entry in transfer["to_sign"]:
            sig = sign_schnorr(key, bytes.fromhex(entry["sighash"]))
            sigs[str(entry["input"])] = sig.hex()
        return self.api("POST", "/v1/transfers/%s/complete" % transfer["id"], {"sigs": sigs})

    # -- test ----------------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101)
        node.sendtoaddress(node.getnewaddress(), 100)
        self.generate(node, 1)

        # Launch openampd against this node.
        url = urlparse(node.url)
        self.amp_port = 18722 + self.options.port_seed % 1000
        datadir = os.path.join(self.options.tmpdir, "openampd")
        proc = subprocess.Popen([
            os.environ["OPENAMPD"],
            "-listen", "127.0.0.1:%d" % self.amp_port,
            "-datadir", datadir,
            "-rpc", "http://%s:%d" % (url.hostname, url.port),
            "-rpcauth", "%s:%s" % (url.username, url.password),
            "-issuertoken", "testtoken",
            "-demoissuer",
            "-feesats", "1000",
            "-follow", "200ms",
        ], stdout=open(os.path.join(self.options.tmpdir, "openampd.log"), "w"), stderr=subprocess.STDOUT)
        try:
            self.run_with_daemon(node)
        finally:
            proc.terminate()
            proc.wait(timeout=10)

    def run_with_daemon(self, node):
        # Wait for the API to come up.
        for _ in range(100):
            try:
                self.api("GET", "/v1/assets")
                break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)

        self.log.info("register participants")
        keys = {}
        aids = {}
        for who in ("alice", "bob", "charlie", "issuer"):
            keys[who] = generate_privkey()
            pub = compute_xonly_pubkey(keys[who])[0].hex()
            aids[who] = self.api("POST", "/v1/users", {"pubkeys": [pub]})["aid"]

        self.log.info("hosted issuance into Alice's enclave")
        res = self.api("POST", "/v1/issuer/assets", {
            "name": "OpenAMP M1 Bond", "ticker": "BND2", "precision": 8,
            "atoms": 100 * COIN, "holder_aid": aids["alice"], "issuer_aid": aids["issuer"],
            "burn_allowed": True,
            "rules": {"fee_convert_atoms": 100},
        }, token="testtoken")
        self.asset = res["asset"]
        issue_txid = res["txid"]
        assert issue_txid in node.getrawmempool()
        self.generate(node, 1)

        self.log.info("re-verify the contract binding independently (python)")
        raw = node.getrawtransaction(issue_txid, True, node.getbestblockhash())
        issuance_vin = raw["vin"][0]
        prevout = COutPoint(int(issuance_vin["txid"], 16), issuance_vin["vout"])
        contract_digest = sha256(json.dumps(json.loads(json.dumps(res["contract"])),
                                            sort_keys=True, separators=(",", ":")).encode())
        _, asset_i, _ = derive_issuance_ids(prevout, contract_digest)
        assert_equal(asset_i[::-1].hex(), self.asset)
        contract = res["contract"]
        assert_equal(contract["openamp"]["policy_pubkey"],
                     self.api("GET", "/v1/assets/" + self.asset)["policy_pub"])

        self.wait_daemon_height(node.getblockcount())
        bal = self.api("GET", "/v1/users/%s/balance?asset=%s" % (aids["alice"], self.asset))
        assert_equal(bal["atoms"], 100 * COIN)

        self.log.info("hosted transfer with fee conversion (Alice -> Bob)")
        t = self.api("POST", "/v1/transfers", {
            "asset": self.asset, "sender_aid": aids["alice"], "recipient_aid": aids["bob"],
            "atoms": 60 * COIN, "fee_mode": "convert",
        })
        assert_equal(t["convert_atoms"], 100)
        res = self.sign_and_complete(t, keys["alice"])
        assert res["txid"] in node.getrawmempool()
        self.generate(node, 1)
        self.wait_daemon_height(node.getblockcount())
        assert_equal(self.api("GET", "/v1/users/%s/balance?asset=%s" % (aids["bob"], self.asset))["atoms"], 60 * COIN)
        assert_equal(self.api("GET", "/v1/users/%s/balance?asset=%s" % (aids["issuer"], self.asset))["atoms"], 100)

        self.log.info("freeze Bob: build succeeds, complete is refused")
        self.api("POST", "/v1/issuer/freeze", {"aid": aids["bob"], "frozen": True}, token="testtoken")
        t = self.api("POST", "/v1/transfers", {
            "asset": self.asset, "sender_aid": aids["bob"], "recipient_aid": aids["alice"],
            "atoms": 1 * COIN, "fee_mode": "sponsor",
        })
        sigs = {str(e["input"]): sign_schnorr(keys["bob"], bytes.fromhex(e["sighash"])).hex() for e in t["to_sign"]}
        err = self.api("POST", "/v1/transfers/%s/complete" % t["id"], {"sigs": sigs}, expect=403)
        assert "frozen" in err["error"]
        self.api("POST", "/v1/issuer/freeze", {"aid": aids["bob"], "frozen": False}, token="testtoken")

        self.log.info("raw co-sign: self-built transaction, self-paid fee")
        addr_info = self.api("GET", "/v1/users/%s/address?asset=%s" % (aids["bob"], self.asset))
        alice_info = self.api("GET", "/v1/users/%s/address?asset=%s" % (aids["alice"], self.asset))
        # Bob's enclave utxo (60 BND2) from the conversion transfer.
        scan = node.scantxoutset("start", ["raw(%s)" % addr_info["script_pubkey"]])
        bob_utxo = [u for u in scan["unspents"] if u["asset"] == self.asset][0]
        fee_utxo = [u for u in node.listunspent() if u["spendable"] and u["amount"] > 1][0]
        tx = CTransaction()
        tx.nVersion = 2
        asset_out = b"\x01" + bytes.fromhex(self.asset)[::-1]
        tx.vin.append(CTxIn(COutPoint(int(bob_utxo["txid"], 16), bob_utxo["vout"])))
        tx.vin.append(CTxIn(COutPoint(int(fee_utxo["txid"], 16), fee_utxo["vout"])))
        tx.vout.append(CTxOut(CTxOutValue(10 * COIN),
                              bytes.fromhex(alice_info["script_pubkey"]), CTxOutAsset(asset_out)))
        tx.vout.append(CTxOut(CTxOutValue(50 * COIN),
                              bytes.fromhex(addr_info["script_pubkey"]), CTxOutAsset(asset_out)))
        fee_in_sats = int(round(float(fee_utxo["amount"]) * COIN))
        change_spk = node.getaddressinfo(node.getnewaddress())["scriptPubKey"]
        tx.vout.append(CTxOut(CTxOutValue(fee_in_sats - 5000), bytes.fromhex(change_spk)))
        tx.vout.append(CTxOut(CTxOutValue(5000)))
        partial = node.signrawtransactionwithwallet(tx.serialize().hex())
        tx = tx_from_hex(partial["hex"])
        while len(tx.wit.vtxinwit) < len(tx.vin):
            tx.wit.vtxinwit.append(CTxInWitness())
        res = self.api("POST", "/v1/cosign", {
            "tx": tx.serialize().hex(), "asset": self.asset,
            "sender_aid": aids["bob"], "inputs": [0],
        })
        entry = res["sigs"][0]
        bob_sig = sign_schnorr(keys["bob"], bytes.fromhex(entry["sighash"]))
        tx.wit.vtxinwit[0].scriptWitness.stack = [
            bytes.fromhex(entry["policy_sig"]), bob_sig,
            bytes.fromhex(entry["leaf"]), bytes.fromhex(entry["control"]),
        ]
        txid = node.sendrawtransaction(tx.serialize().hex())
        self.generate(node, 1)
        self.wait_daemon_height(node.getblockcount())

        self.log.info("velocity limit refusal")
        self.api("POST", "/v1/issuer/rules", {
            "asset": self.asset,
            "rules": {"fee_convert_atoms": 100, "velocity_window_blocks": 100, "velocity_max_atoms": 5 * COIN},
        }, token="testtoken")
        t = self.api("POST", "/v1/transfers", {
            "asset": self.asset, "sender_aid": aids["alice"], "recipient_aid": aids["bob"],
            "atoms": 6 * COIN, "fee_mode": "sponsor",
        })
        sigs = {str(e["input"]): sign_schnorr(keys["alice"], bytes.fromhex(e["sighash"])).hex() for e in t["to_sign"]}
        err = self.api("POST", "/v1/transfers/%s/complete" % t["id"], {"sigs": sigs}, expect=403)
        assert "velocity" in err["error"]

        self.log.info("holder cap refusal")
        self.api("POST", "/v1/issuer/rules", {
            "asset": self.asset,
            "rules": {"fee_convert_atoms": 100, "holder_cap": 3},
        }, token="testtoken")
        t = self.api("POST", "/v1/transfers", {
            "asset": self.asset, "sender_aid": aids["alice"], "recipient_aid": aids["charlie"],
            "atoms": 1 * COIN, "fee_mode": "sponsor",
        })
        sigs = {str(e["input"]): sign_schnorr(keys["alice"], bytes.fromhex(e["sighash"])).hex() for e in t["to_sign"]}
        err = self.api("POST", "/v1/transfers/%s/complete" % t["id"], {"sigs": sigs}, expect=403)
        assert "holder cap" in err["error"]
        self.api("POST", "/v1/issuer/rules", {"asset": self.asset, "rules": {"fee_convert_atoms": 100}},
                 token="testtoken")

        self.log.info("ownership report")
        report = self.api("GET", "/v1/issuer/holders?asset=" + self.asset, token="testtoken")
        # Supply is conserved: alice 100 - 60 - 100atoms + 10 = 50*COIN - 100;
        # bob 60 - 10 = 50*COIN; issuer holds the 100 converted atoms.
        assert_equal(report["total_atoms"], 100 * COIN)
        assert_equal(report["holders"][aids["alice"]], 50 * COIN - 100)
        assert_equal(report["holders"][aids["bob"]], 50 * COIN)
        assert_equal(report["holders"][aids["issuer"]], 100)

        self.log.info("clawback via L_claw")
        res = self.api("POST", "/v1/issuer/clawback", {
            "asset": self.asset, "holder_aid": aids["bob"], "reason": "court order demo",
        }, token="testtoken")
        assert res["txid"] in node.getrawmempool()
        self.generate(node, 1)
        self.wait_daemon_height(node.getblockcount())
        assert_equal(self.api("GET", "/v1/users/%s/balance?asset=%s" % (aids["bob"], self.asset))["atoms"], 0)

        self.log.info("transparency log: verify the hash chain, then anchor it on-chain")
        log_lines = [json.loads(l) for l in
                     self.api_raw("/v1/log").strip().splitlines()]
        prev = ""
        for e in log_lines:
            assert_equal(e["prev"], prev)
            data_bytes = json.dumps(e["data"], separators=(",", ":")).encode()
            pre = ("%d|%s|%s|%s|" % (e["seq"], e["prev"], e["time"], e["action"])).encode() + data_bytes
            assert_equal(sha256(pre).hex(), e["hash"])
            prev = e["hash"]
        actions = [e["action"] for e in log_lines]
        for expected in ("register", "issue", "transfer", "freeze", "cosign", "clawback"):
            assert expected in actions, "missing %s in transparency log" % expected
        res = self.api("POST", "/v1/issuer/anchor", None, token="testtoken")
        anchor_txid = res["txid"]
        self.generate(node, 1)
        raw = node.getrawtransaction(anchor_txid, True, node.getbestblockhash())
        payloads = [v["scriptPubKey"]["hex"] for v in raw["vout"] if v["scriptPubKey"]["hex"].startswith("6a")]
        assert any("4f50454e414d50" in p for p in payloads), "OPENAMP marker not in OP_RETURN"

        self.log.info("M1+M3 daemon e2e complete")

    def api_raw(self, path):
        conn = http.client.HTTPConnection("127.0.0.1", self.amp_port, timeout=30)
        conn.request("GET", path)
        resp = conn.getresponse()
        data = resp.read().decode()
        conn.close()
        assert_equal(resp.status, 200)
        return data


if __name__ == "__main__":
    OpenAmpDaemonTest().main()
