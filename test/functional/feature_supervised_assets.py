#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""SEQUENTIA: supervised assets, end to end.

A supervised asset is one whose issuer can freeze holders by consensus rule.
The design and the decisions behind it are in
doc/sequentia/supervised-assets-implementation.md; this exercises the whole
pipeline on a live chain, because most of what can go wrong here is only
visible once the registry, the mempool and block production are all involved.

Covered, in the order the test runs them:

  1. Supervision is committed IN the asset id, so the same issuance with
     different keys is a different asset, and an unsupervised issuance is
     unaffected.
  2. Issuing a supervised asset, and the chain learning about it.
  3. The rules that cannot be repaired later, so they are refused at issuance:
     reissuance tokens required, no blinding.
  4. A freeze needs the issuer's current key. Nobody else's will do.
  5. A frozen holder cannot spend, and CAN still be paid.
  6. A freeze does not reach shared scripts. This is the property that lets
     supervised assets exist on Lightning and on the DEX at all.
  7. Unfreezing, by spending the record.
  8. Key rotation, and the trap it exists to close: after rotating, the old key
     neither freezes nor unfreezes.
  9a. A spend the freeze refuses is reported as refused, and stops holding its
     inputs hostage. Left as an ordinary pending payment it reads as one still
     on its way, while the asset AND the fee it used vanish from the balance.
  9b. A valid transaction that simply has not been relayed is left strictly
     alone -- it is the case that looks identical from the wallet's side and
     that must never be treated as refused.
  9. A freeze evicts the spends it invalidated from the mempool. Without this
     every block template would re-select them, every template would fail
     validation, and the chain would stop making blocks.
 10. A reorg takes the freeze back with it, exactly.
 11. Pause: one record stops every holding of an asset at once, but only if the
     asset was issued with the capability, which is visible in its id.
 12. The private submission channel: a freeze reaches a block without ever
     appearing in a mempool, which is what stops every freeze being front-run.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.key import ECKey, compute_xonly_pubkey, sign_schnorr
from test_framework.util import assert_equal, assert_raises_rpc_error

from decimal import Decimal


def make_key(seed):
    """A deterministic key, and its x-only public key in hex."""
    key = ECKey()
    key.set(seed.to_bytes(32, "big"), True)
    assert key.is_valid
    xonly, _ = compute_xonly_pubkey(key.get_bytes())
    return key, xonly.hex()


def schnorr(key, sighash_hex):
    return sign_schnorr(key.get_bytes(), bytes.fromhex(sighash_hex)).hex()


class SupervisedAssetsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[
            "-con_default_blinded_addresses=0",
            "-blindedaddresses=0",
            "-initialfreecoins=10000000000",
            "-con_blocksubsidy=0",
            "-con_connect_genesis_outputs=1",
            "-anyonecanspendaremine=1",
            "-txindex=1",
            "-supervisedassetsheight=1",
        ]] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # -- helpers ----------------------------------------------------------

    def funding_outpoint(self, node, minimum=Decimal("1")):
        """An unspent policy-asset output to build a raw transaction on."""
        for utxo in node.listunspent():
            if utxo["asset"] == self.policy_asset and utxo["amount"] > minimum:
                return utxo
        raise AssertionError("no funding utxo")

    def send_raw(self, node, funded_hex):
        signed = node.signrawtransactionwithwallet(funded_hex)
        assert signed["complete"], signed
        return node.sendrawtransaction(signed["hex"])

    def record_tx(self, node, kind, asset, target, signer, oldkey=None):
        """Build, sign and broadcast a supervision record.

        Mirrors what a real issuer does: the node says what to sign, the key
        signs it wherever it lives, and the node assembles the result. No
        private key ever reaches the node.
        """
        utxo = self.funding_outpoint(node)
        sighash = node.getsupervisionrecordhash(
            kind, asset, target, oldkey, utxo["txid"], utxo["vout"])["sighash"]
        built = node.buildsupervisionrecord(kind, asset, target, oldkey, schnorr(signer, sighash))

        change = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{change: utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        # The record output carries zero of the asset it governs.
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        return self.send_raw(node, raw), built["targethash"]

    def trusted_policy_balance(self, node):
        """The node's spendable policy asset.

        getbalances() keys an asset by its LABEL where it has one, and only by
        its hex id where it does not, so the policy asset arrives as "bitcoin"
        rather than as self.policy_asset. An asset with nothing spendable is
        absent from the map altogether rather than present as zero.
        """
        balances = node.getbalances()["mine"]["trusted"]
        return balances.get("bitcoin", balances.get(self.policy_asset, 0))

    def spend_asset(self, node, asset, amount, destination):
        """Spend `amount` of `asset` from node's wallet to `destination`."""
        raw = node.createrawtransaction([], [{destination: amount, "asset": asset}])
        # The open fee market has no default fee asset, so name it.
        funded = node.fundrawtransaction(raw, {"fee_asset": self.policy_asset})["hex"]
        return self.send_raw(node, funded)

    # -- the test ---------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, COINBASE_MATURITY + 1)
        self.sync_all()
        self.policy_asset = node.dumpassetlabels()["bitcoin"]

        # The holder node needs the fee asset of its own, or its spends fail for
        # want of funds before consensus ever sees them.
        self.spend_asset(node, self.policy_asset, 20, self.nodes[1].getnewaddress())
        self.generate(node, 1)
        self.sync_all()

        self.operational, self.op_pub = make_key(0x1111)
        self.recovery, self.rec_pub = make_key(0x2222)

        self.test_descriptor_is_in_the_asset_id(node)
        asset, token, entropy = self.test_issue(node)
        self.test_issuance_refusals(node)
        self.test_freeze_needs_the_issuers_key(node, asset)
        holder = self.test_freeze_blocks_the_holder(node, asset)
        self.test_shared_scripts_are_out_of_reach(node, asset)
        self.test_unfreeze(node, asset, holder)
        self.test_rotation_closes_the_trap(node, asset, holder)
        self.test_mempool_eviction(node, asset)
        self.test_a_refusal_is_reported_and_frees_its_funds(node, asset)
        self.test_reorg_takes_the_freeze_with_it(node, asset)
        self.test_pause(node)
        self.test_private_submission(node)
        self.test_wallet_issuance(node)

    def test_descriptor_is_in_the_asset_id(self, node):
        self.log.info("The descriptor is committed in the asset id")
        utxo = self.funding_outpoint(node)
        base = node.getsupervisedassetid(utxo["txid"], utxo["vout"], self.op_pub, self.rec_pub)

        # Deterministic.
        again = node.getsupervisedassetid(utxo["txid"], utxo["vout"], self.op_pub, self.rec_pub)
        assert_equal(base["asset"], again["asset"])

        # A different operational key is a DIFFERENT ASSET. This is what makes
        # supervision impossible to add, remove or alter after issuance, and
        # why the keys have to be chosen before anything is issued.
        _, other_pub = make_key(0x3333)
        assert base["asset"] != node.getsupervisedassetid(
            utxo["txid"], utxo["vout"], other_pub, self.rec_pub)["asset"]
        # ...and so is a different recovery key.
        assert base["asset"] != node.getsupervisedassetid(
            utxo["txid"], utxo["vout"], self.op_pub, other_pub)["asset"]

        # The keys must be real, and distinct: equal keys would collapse the
        # separation between using the authority and rotating it.
        assert_raises_rpc_error(-8, "must differ", node.getsupervisedassetid,
                                utxo["txid"], utxo["vout"], self.op_pub, self.op_pub)
        assert_raises_rpc_error(-8, "x-only", node.getsupervisedassetid,
                                utxo["txid"], utxo["vout"], "00" * 32, self.rec_pub)

        # The declaration output is unspendable but not prunable, because the
        # supervised-asset set is read back out of the UTXO set.
        decoded = node.decodesupervisionscript(base["declarationscript"])
        assert_equal(decoded["type"], "declaration")
        assert_equal(decoded["asset"], base["asset"])
        assert_equal(decoded["operationalkey"], self.op_pub)
        assert_equal(decoded["recoverykey"], self.rec_pub)

    def test_issue(self, node):
        self.log.info("Issuing a supervised asset")
        assert_equal(node.getsupervisedassets(), [])

        addr = node.getnewaddress()
        token_addr = node.getnewaddress()
        raw = node.createrawtransaction([], [{node.getnewaddress(): Decimal("0.9")},
                                             {"fee": Decimal("0.001")}])
        funded = node.fundrawtransaction(raw)["hex"]
        issued = node.rawissueasset(funded, [{
            "asset_amount": 1000,
            "asset_address": addr,
            "token_amount": 1,
            "token_address": token_addr,
            "blind": False,
            "supervision": {"operationalkey": self.op_pub, "recoverykey": self.rec_pub},
        }])[0]

        self.send_raw(node, issued["hex"])
        self.generate(node, 1)
        self.sync_all()

        assets = node.getsupervisedassets()
        assert_equal(len(assets), 1)
        assert_equal(assets[0]["asset"], issued["asset"])
        assert_equal(assets[0]["operationalkey"], self.op_pub)
        assert_equal(assets[0]["recoverykey"], self.rec_pub)
        assert_equal(assets[0]["frozen"], 0)
        # The issued keys are what the id commits to and never change; the
        # current keys start there and move only by rotation.
        assert_equal(assets[0]["issuedoperationalkey"], self.op_pub)

        # The other node derives the same registry from the same blocks.
        assert_equal(self.nodes[1].getsupervisedassets(), assets)

        assert_equal(node.getbalance()[issued["asset"]], Decimal("1000"))
        self.log.info("  asset %s", issued["asset"])
        return issued["asset"], issued["token"], issued["entropy"]

    def test_issuance_refusals(self, node):
        self.log.info("Rules that cannot be repaired later are refused at issuance")
        raw = node.createrawtransaction([], [{node.getnewaddress(): Decimal("0.9")},
                                             {"fee": Decimal("0.001")}])
        funded = node.fundrawtransaction(raw)["hex"]
        supervision = {"operationalkey": self.op_pub, "recoverykey": self.rec_pub}

        # No reissuance tokens. Seize and burn are answered by freeze-plus-
        # reissue rather than by giving an issuer a spending power, so an asset
        # that cannot be reissued must not be able to call itself supervised.
        assert_raises_rpc_error(-8, "reissuance tokens", node.rawissueasset, funded, [{
            "asset_amount": 10, "asset_address": node.getnewaddress(),
            "blind": False, "supervision": supervision}])

        # Blinded. Consensus cannot read a blinded output's asset, so a
        # supervised asset in one would be unfreezable and unauditable.
        assert_raises_rpc_error(-8, "cannot be blinded", node.rawissueasset, funded, [{
            "asset_amount": 10, "asset_address": node.getnewaddress(),
            "token_amount": 1, "token_address": node.getnewaddress(),
            "blind": True, "supervision": supervision}])

    def test_freeze_needs_the_issuers_key(self, node, asset):
        self.log.info("A freeze needs the issuer's current key")
        target = node.getnewaddress()
        utxo = self.funding_outpoint(node)
        sighash = node.getsupervisionrecordhash(
            "freeze", asset, target, None, utxo["txid"], utxo["vout"])["sighash"]
        assert_equal(node.getsupervisionrecordhash(
            "freeze", asset, target, None, utxo["txid"], utxo["vout"])["signwith"], "operational")

        # Signed by a stranger.
        stranger, _ = make_key(0x4444)
        built = node.buildsupervisionrecord("freeze", asset, target, None, schnorr(stranger, sighash))
        change = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{change: utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        signed = node.signrawtransactionwithwallet(raw)
        assert_raises_rpc_error(-26, "supervision-record", node.sendrawtransaction, signed["hex"])

        # Signed by the RECOVERY key, which may rotate but must never freeze.
        built = node.buildsupervisionrecord("freeze", asset, target, None, schnorr(self.recovery, sighash))
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{change: utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        signed = node.signrawtransactionwithwallet(raw)
        assert_raises_rpc_error(-26, "supervision-record", node.sendrawtransaction, signed["hex"])

    def test_freeze_blocks_the_holder(self, node, asset):
        self.log.info("A frozen holder cannot spend, but can still be paid")
        holder = self.nodes[1].getnewaddress()
        self.spend_asset(node, asset, Decimal("100"), holder)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(self.nodes[1].getbalance()[asset], Decimal("100"))

        status = node.isassetfrozen(asset, holder)
        assert_equal(status["frozen"], False)
        assert_equal(status["freezable"], True)

        self.freeze_txid, self.freeze_target = self.record_tx(
            node, "freeze", asset, holder, self.operational)
        self.generate(node, 1)
        self.sync_all()

        assert_equal(node.isassetfrozen(asset, holder)["frozen"], True)
        assert_equal(node.getassetfreezes(asset), [{"targethash": self.freeze_target, "records": 1}])
        assert_equal(self.nodes[1].isassetfrozen(asset, holder)["frozen"], True)

        # The holder cannot spend.
        assert_raises_rpc_error(-26, "asset-frozen", self.spend_asset,
                                self.nodes[1], asset, Decimal("10"), node.getnewaddress())

        # ...but can still be PAID. Enforcing at output creation instead would
        # let a frozen party poison any address by sending to it.
        self.spend_asset(node, asset, Decimal("5"), holder)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(self.nodes[1].getbalance()[asset], Decimal("105"))
        return holder

    def test_shared_scripts_are_out_of_reach(self, node, asset):
        self.log.info("A freeze does not reach shared scripts")
        # A 2-of-2, which is the shape of a Lightning funding output. Freezing
        # it would strand a counterparty who did nothing, and freeze a contract
        # whose timelocks keep running.
        a = node.getaddressinfo(node.getnewaddress())["pubkey"]
        b = node.getaddressinfo(node.getnewaddress())["pubkey"]
        shared = node.createmultisig(2, [a, b], "bech32")["address"]

        status = node.isassetfrozen(asset, shared)
        assert_equal(status["freezable"], False)

        # The record can still be created -- consensus does not know what a
        # target will turn out to be -- but it binds nothing.
        self.spend_asset(node, asset, Decimal("10"), shared)
        self.generate(node, 1)
        self.sync_all()
        self.record_tx(node, "freeze", asset, shared, self.operational)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(node.isassetfrozen(asset, shared)["frozen"], True)
        assert_equal(node.isassetfrozen(asset, shared)["freezable"], False)

    def test_unfreeze(self, node, asset, holder):
        self.log.info("Unfreezing, by spending the record")
        record = self.find_record(node, self.freeze_txid)
        sighash = node.getsupervisionunfreezehash(
            self.freeze_txid, record, asset, self.freeze_target)

        # The wrong key cannot lift it.
        stranger, _ = make_key(0x5555)
        assert_raises_rpc_error(-26, "supervision-unfreeze", self.unfreeze_with,
                                node, self.freeze_txid, record, schnorr(stranger, sighash))

        self.unfreeze_with(node, self.freeze_txid, record, schnorr(self.operational, sighash))
        self.generate(node, 1)
        self.sync_all()

        assert_equal(node.isassetfrozen(asset, holder)["frozen"], False)
        # ...and the holder can spend again.
        self.spend_asset(self.nodes[1], asset, Decimal("10"), node.getnewaddress())
        self.generate(node, 1)
        self.sync_all()

    def find_record(self, node, txid):
        """The output index of the supervision record in `txid`."""
        raw = node.getrawtransaction(txid, True)
        for out in raw["vout"]:
            decoded = node.decodesupervisionscript(out["scriptPubKey"]["hex"])
            if decoded["type"] == "record":
                return out["n"]
        raise AssertionError("no record output in %s" % txid)

    def unfreeze_with(self, node, txid, vout, signature):
        """Spend a freeze record, which is the unfreeze."""
        utxo = self.funding_outpoint(node)
        change = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": txid, "vout": vout}, {"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{change: utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        signed = node.signrawtransactionwithwallet(raw)
        # The record's own script does not authorise the spend; consensus does,
        # against the CURRENT operational key. The signature rides in the
        # scriptSig as a single push.
        decoded = node.decoderawtransaction(signed["hex"])
        signed_hex = signed["hex"]
        for i, vin in enumerate(decoded["vin"]):
            if vin["txid"] == txid and vin["vout"] == vout:
                signed_hex = node.setsupervisionunfreezesig(signed_hex, i, signature)
                break
        return node.sendrawtransaction(signed_hex)

    def test_rotation_closes_the_trap(self, node, asset, holder):
        self.log.info("Rotation, and the trap it closes")
        fresh, fresh_pub = make_key(0x6666)

        # The operational key cannot rotate anything, not even itself. This
        # asymmetry is the whole reason for a second key: a thief who has the
        # operational key can grief, visibly, but can never take the authority.
        utxo = self.funding_outpoint(node)
        sighash = node.getsupervisionrecordhash(
            "rotateoperational", asset, fresh_pub, self.op_pub, utxo["txid"], utxo["vout"])
        assert_equal(sighash["signwith"], "recovery")
        built = node.buildsupervisionrecord("rotateoperational", asset, fresh_pub, self.op_pub,
                                            schnorr(self.operational, sighash["sighash"]))
        change = node.getnewaddress()
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{change: utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        signed = node.signrawtransactionwithwallet(raw)
        assert_raises_rpc_error(-26, "supervision-record", node.sendrawtransaction, signed["hex"])

        # The recovery key can.
        self.record_tx(node, "rotateoperational", asset, fresh_pub, self.recovery, self.op_pub)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(node.getsupervisedassets()[0]["operationalkey"], fresh_pub)
        # The committed key is unchanged: it is in the asset id and cannot move.
        assert_equal(node.getsupervisedassets()[0]["issuedoperationalkey"], self.op_pub)

        # THE TRAP. The old key now signs nothing, which is the entire point of
        # rotating after a compromise.
        utxo = self.funding_outpoint(node)
        sighash = node.getsupervisionrecordhash(
            "freeze", asset, holder, None, utxo["txid"], utxo["vout"])["sighash"]
        built = node.buildsupervisionrecord("freeze", asset, holder, None,
                                            schnorr(self.operational, sighash))
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{node.getnewaddress(): utxo["amount"] - Decimal("0.001")}, {"fee": Decimal("0.001")}])
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        signed = node.signrawtransactionwithwallet(raw)
        assert_raises_rpc_error(-26, "supervision-record", node.sendrawtransaction, signed["hex"])

        # The new key works.
        self.operational, self.op_pub = fresh, fresh_pub
        self.record_tx(node, "freeze", asset, holder, self.operational)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(node.isassetfrozen(asset, holder)["frozen"], True)

    def test_mempool_eviction(self, node, asset):
        self.log.info("A freeze evicts the spends it invalidated")
        victim = self.nodes[1].getnewaddress()
        self.spend_asset(node, asset, Decimal("50"), victim)
        self.generate(node, 1)
        self.sync_all()

        # What the holder has before spending anything, so the next test can say
        # whether the refused spend gave it all back.
        self.balance_before_refused = self.nodes[1].getbalances()["mine"]["trusted"]

        # A spend that is perfectly valid right now.
        pending = self.spend_asset(self.nodes[1], asset, Decimal("20"), node.getnewaddress())
        assert pending in self.nodes[1].getrawmempool()
        self.refused_txid = pending

        # Freeze the script this transaction actually spends, whichever of the
        # holder's outputs its wallet happened to pick.
        raw = self.nodes[1].getrawtransaction(pending, True)
        target_script = None
        for vin in raw["vin"]:
            prev = self.nodes[1].getrawtransaction(vin["txid"], True)["vout"][vin["vout"]]
            if prev.get("asset") == asset:
                target_script = prev["scriptPubKey"]["hex"]
                break
        assert target_script is not None, "the pending spend does not move the asset"

        # Frozen from the other node, so the eviction is not a local artefact of
        # the node that made the record.
        self.record_tx(node, "freeze", asset, target_script, self.operational)
        self.generate(node, 1)
        self.sync_all()

        # The pending spend is gone. Left resident it would be re-selected into
        # every block template, every template would fail validation, and every
        # producer would skip its slot.
        assert pending not in self.nodes[1].getrawmempool()
        assert pending not in node.getrawmempool()

        # And the chain keeps making blocks, which is the property that actually
        # matters here.
        height = node.getblockcount()
        self.generate(node, 2)
        self.sync_all()
        assert_equal(node.getblockcount(), height + 2)

    def test_a_refusal_is_reported_and_frees_its_funds(self, node, asset):
        """The two opposite cases a wallet cannot tell apart on its own.

        After the eviction above, the holder's wallet is in the exact state that
        was reported: a transaction it made, unconfirmed, in nobody's mempool,
        not abandoned. Consensus will refuse it for as long as the freeze
        stands, but nothing in the wallet said so, and the inputs it reserved --
        the asset and the fee asset both -- were simply gone from the balance.

        The opposite case is a transaction that is perfectly valid and has just
        not been relayed yet. It looks the same from here, and it must be left
        exactly as Bitcoin Core leaves it: pending, with its inputs reserved.
        """
        self.log.info("A refused spend is reported as refused and frees its funds")
        holder = self.nodes[1]

        # The state under test, before anything is asserted about it.
        entry = holder.gettransaction(self.refused_txid)
        assert_equal(entry["confirmations"], 0)
        assert_equal(entry["details"][0]["abandoned"], False)
        assert self.refused_txid not in holder.getrawmempool()

        # And the node does refuse it, with the reason the user has to be told.
        verdict = holder.testmempoolaccept([entry["hex"]])[0]
        assert_equal(verdict["allowed"], False)
        assert_equal(verdict["reject-reason"], "bad-txns-asset-frozen")

        # The wallet asks the node on a timer, so give it one round to notice.
        # What it must conclude is that the inputs are not spent after all:
        # every asset the refused spend touched comes back, the fee asset
        # included, which is the half of the report that had no explanation.
        self.wait_until(
            lambda: holder.getbalances()["mine"]["trusted"] == self.balance_before_refused,
            timeout=90)

        # It is reported, not silently repaired -- and reported as consensus
        # refusing it, which is what justifies releasing the inputs at all.
        holder.assert_debug_log(
            expected_msgs=["Transaction {} refused by the node: bad-txns-asset-frozen "
                           "(inputs released)".format(self.refused_txid)],
            timeout=1)

        # Nothing was abandoned on the user's behalf. The record is still here,
        # and lifting the freeze must be able to bring it back.
        entry = holder.gettransaction(self.refused_txid)
        assert_equal(entry["details"][0]["abandoned"], False)

        self.log.info("A valid spend that was merely never relayed is left alone")
        # Give the holder a confirmed coin to pay with first. Everything it has
        # at this point is change of the refused family: released back into the
        # balance, but unconfirmed and therefore not trusted, so coin selection
        # has nothing to spend and the control could not be built at all.
        self.spend_asset(node, self.policy_asset, Decimal("5"), holder.getnewaddress())
        self.generate(node, 1)
        self.sync_all()

        # -walletbroadcast=0 reproduces the other case exactly: the wallet
        # commits the transaction and never submits it, so it sits unconfirmed
        # and outside every mempool while being entirely valid.
        self.restart_node(1, self.extra_args[1] + ["-walletbroadcast=0"])
        self.connect_nodes(0, 1)
        self.sync_all()

        before = self.trusted_policy_balance(holder)
        assert before > 0, "the control needs a confirmed coin to pay with"
        unrelayed = holder.sendtoaddress(address=node.getnewaddress(), amount=Decimal("1"),
                                         fee_asset_label=self.policy_asset)
        assert unrelayed not in holder.getrawmempool()
        assert unrelayed not in node.getrawmempool()
        assert_equal(holder.gettransaction(unrelayed)["confirmations"], 0)

        # Two full rounds of the sweep, so "it has not got round to it yet"
        # cannot be mistaken for "it decided to leave this one alone".
        holder.assert_debug_log(
            expected_msgs=[],
            unexpected_msgs=["Transaction {} refused by the node".format(unrelayed)],
            timeout=45)

        # Still reserved, exactly as before this change: a transaction that may
        # yet be relayed and mined has not released anything.
        assert self.trusted_policy_balance(holder) < before

        # Abandon it before restoring the node. Left alone it would be broadcast
        # the moment the wallet is allowed to again, and every test after this
        # one would be running against a mempool holding a transaction that only
        # exists to make a point about relay.
        holder.abandontransaction(unrelayed)

        # Put the holder back the way the remaining tests expect to find it.
        self.restart_node(1, self.extra_args[1])
        self.connect_nodes(0, 1)
        # Blocks only. sync_all() also waits for the mempools to match, which is
        # the one thing this scenario cannot promise: the node spent the last
        # minute deliberately not relaying, and what the tests after this need
        # from it is a shared tip, not a shared mempool.
        self.sync_blocks()

    def test_reorg_takes_the_freeze_with_it(self, node, asset):
        self.log.info("A reorg takes the freeze back with it")
        target = node.getnewaddress()
        self.disconnect_nodes(0, 1)

        _, target_hash = self.record_tx(node, "freeze", asset, target, self.operational)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.isassetfrozen(asset, target)["frozen"], True)
        frozen_before = len(node.getassetfreezes(asset))

        # Build a longer chain on the other node and reconnect. The freeze was
        # only ever in the abandoned branch, so it must vanish exactly.
        self.generate(self.nodes[1], 3, sync_fun=self.no_op)
        self.connect_nodes(0, 1)
        self.sync_blocks()

        assert_equal(node.isassetfrozen(asset, target)["frozen"], False)
        assert_equal(len(node.getassetfreezes(asset)), frozen_before - 1)
        # The asset itself survives: its declaration is in a block both
        # branches share.
        assert_equal(len(node.getsupervisedassets()), 1)
        assert_equal(node.getsupervisedassets(), self.nodes[1].getsupervisedassets())


    def issue_asset(self, node, pause=False):
        """Issue a supervised asset, optionally with the pause capability."""
        raw = node.createrawtransaction([], [{node.getnewaddress(): Decimal("0.9")},
                                             {"fee": Decimal("0.001")}])
        funded = node.fundrawtransaction(raw)["hex"]
        supervision = {"operationalkey": self.op_pub, "recoverykey": self.rec_pub}
        if pause:
            supervision["pause"] = True
        issued = node.rawissueasset(funded, [{
            "asset_amount": 1000,
            "asset_address": node.getnewaddress(),
            "token_amount": 1,
            "token_address": node.getnewaddress(),
            "blind": False,
            "supervision": supervision,
        }])[0]
        self.send_raw(node, issued["hex"])
        self.generate(node, 1)
        self.sync_all()
        return issued["asset"]

    def test_pause(self, node):
        self.log.info("Pause stops every holding at once, if the asset allows it")
        # Top the holder node up: earlier scenarios spent its fee asset, and a
        # spend that fails for want of funds never reaches the rule under test.
        self.spend_asset(node, self.policy_asset, 10, self.nodes[1].getnewaddress())
        self.generate(node, 1)
        self.sync_all()

        # An asset issued WITHOUT the capability cannot be paused, ever. The bit
        # is in the id, so this is not a policy anyone can change later.
        plain = self.issue_asset(node, pause=False)
        entry = [a for a in node.getsupervisedassets() if a["asset"] == plain][0]
        assert_equal(entry["pauseallowed"], False)
        assert_raises_rpc_error(-26, "supervision-record", self.record_tx,
                                node, "pause", plain, None, self.operational)

        # An asset issued WITH it. Note the ids differ: a holder can see from
        # the asset alone whether what they accept can be stopped wholesale.
        pausable = self.issue_asset(node, pause=True)
        assert pausable != plain
        entry = [a for a in node.getsupervisedassets() if a["asset"] == pausable][0]
        assert_equal(entry["pauseallowed"], True)
        assert_equal(entry["paused"], False)

        # Two holders, neither of them named by anything.
        alice = self.nodes[1].getnewaddress()
        bob = self.nodes[1].getnewaddress()
        self.spend_asset(node, pausable, Decimal("100"), alice)
        self.spend_asset(node, pausable, Decimal("100"), bob)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(node.isassetfrozen(pausable, alice)["frozen"], False)

        pause_txid, _ = self.record_tx(node, "pause", pausable, None, self.operational)
        self.generate(node, 1)
        self.sync_all()

        # One record, and BOTH are frozen without either being named.
        for who in (alice, bob):
            status = node.isassetfrozen(pausable, who)
            assert_equal(status["frozen"], True)
            assert_equal(status["paused"], True)
        assert_equal(node.getsupervisedassets()[0]["asset"] is not None, True)
        assert_equal([a for a in node.getsupervisedassets()
                      if a["asset"] == pausable][0]["paused"], True)
        assert_raises_rpc_error(-26, "asset-frozen", self.spend_asset,
                                self.nodes[1], pausable, Decimal("10"), node.getnewaddress())

        # The pause is confined to its own asset: the earlier one is untouched.
        assert_equal(node.isassetfrozen(plain, alice)["paused"], False)

        # Lifting it is spending the record, exactly like any other freeze.
        record = self.find_record(node, pause_txid)
        sighash = node.getsupervisionunfreezehash(pause_txid, record, pausable,
                                                  "00" * 32)
        self.unfreeze_with(node, pause_txid, record, schnorr(self.operational, sighash))
        self.generate(node, 1)
        self.sync_all()
        assert_equal(node.isassetfrozen(pausable, alice)["paused"], False)
        assert_equal(node.isassetfrozen(pausable, alice)["frozen"], False)
        self.spend_asset(self.nodes[1], pausable, Decimal("10"), node.getnewaddress())
        self.generate(node, 1)
        self.sync_all()

    def build_record_tx(self, node, kind, asset, target, signer, oldkey=None):
        """Build and sign a record transaction without broadcasting it."""
        utxo = self.funding_outpoint(node)
        sighash = node.getsupervisionrecordhash(
            kind, asset, target, oldkey, utxo["txid"], utxo["vout"])["sighash"]
        built = node.buildsupervisionrecord(kind, asset, target, oldkey, schnorr(signer, sighash))
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            [{node.getnewaddress(): utxo["amount"] - Decimal("0.001")},
             {"fee": Decimal("0.001")}])
        raw = node.addsupervisionrecordoutput(raw, built["script"], asset)
        signed = node.signrawtransactionwithwallet(raw)
        assert signed["complete"], signed
        return signed["hex"], built["targethash"]

    def test_private_submission(self, node):
        self.log.info("A freeze reaches a block without ever entering a mempool")
        self.spend_asset(node, self.policy_asset, 5, self.nodes[1].getnewaddress())
        self.generate(node, 1)
        self.sync_all()

        asset = self.issue_asset(node)
        victim = self.nodes[1].getnewaddress()
        self.spend_asset(node, asset, Decimal("100"), victim)
        self.generate(node, 1)
        self.sync_all()

        raw, target_hash = self.build_record_tx(node, "freeze", asset, victim, self.operational)

        # The channel carries supervision records and nothing else. A private
        # path into block templates for ordinary transactions would be a
        # censorship and ordering lever, so it is refused by shape.
        ordinary = node.createrawtransaction([], [{node.getnewaddress(): Decimal("0.5")}])
        ordinary = node.fundrawtransaction(ordinary, {"fee_asset": self.policy_asset})["hex"]
        ordinary = node.signrawtransactionwithwallet(ordinary)["hex"]
        assert_raises_rpc_error(-8, "supervision records only",
                                node.submitsupervisionrecord, ordinary)

        submitted = node.submitsupervisionrecord(raw)
        txid = submitted["txid"]
        assert_equal(submitted["queued"], 1)
        assert_equal(node.getsupervisionsubmissions()[0]["txid"], txid)

        # Invisible. Not in this node's mempool, and never announced, so the
        # other node has no way to know the target is about to be frozen.
        assert txid not in node.getrawmempool()
        self.sync_all()
        assert txid not in self.nodes[1].getrawmempool()
        assert_equal(self.nodes[1].getsupervisionsubmissions(), [])
        assert_equal(node.isassetfrozen(asset, victim)["frozen"], False)

        # It lands in the very next block this producer makes.
        self.generate(node, 1)
        self.sync_all()
        block = node.getblock(node.getbestblockhash())
        assert txid in block["tx"], "the submission was not included"
        assert_equal(node.isassetfrozen(asset, victim)["frozen"], True)
        assert_equal(self.nodes[1].isassetfrozen(asset, victim)["frozen"], True)

        # And the queue lets it go once it is confirmed.
        assert_equal(node.getsupervisionsubmissions(), [])

        # Resubmitting a spent one is refused rather than held for ever.
        assert_raises_rpc_error(-26, "", node.submitsupervisionrecord, raw)

    def test_wallet_issuance(self, node):
        """The path the Qt wallet takes: issueasset with a supervision object."""
        self.log.info("The wallet can issue a supervised asset directly")
        _, op_pub = make_key(0x7777)
        _, rec_pub = make_key(0x8888)

        r = node.issueasset(100, 1, False, None, self.policy_asset, 8, None,
                            {"operationalkey": op_pub, "recoverykey": rec_pub, "pause": True})
        assert_equal(r["supervised"], True)
        assert_equal(r["operationalkey"], op_pub)
        assert_equal(r["pauseallowed"], True)
        self.generate(node, 1)
        self.sync_all()

        entry = [a for a in node.getsupervisedassets() if a["asset"] == r["asset"]]
        assert_equal(len(entry), 1)
        assert_equal(entry[0]["operationalkey"], op_pub)
        assert_equal(entry[0]["pauseallowed"], True)
        # The wallet built it fully explicit, or consensus would have refused it.
        assert_equal(node.getbalance()[r["asset"]], Decimal("100"))

        # The wallet must READ its own supervised issuance back correctly, not only
        # write it. A supervised asset id descends from a different entropy, so
        # anything deriving the ordinary way reports an asset that does not exist:
        # listissuances named a stranger, and reissueasset could not find the
        # reissuance token of an asset consensus GUARANTEES has one -- which would
        # have left freeze-plus-reissue, the whole answer to a seizure order,
        # impossible from the wallet that issued the asset.
        issuance = [i for i in node.listissuances() if i["asset"] == r["asset"]]
        assert_equal(len(issuance), 1)
        assert_equal(issuance[0]["token"], r["token"])
        assert_equal(issuance[0]["entropy"], r["entropy"])

        node.reissueasset(r["asset"], 50, self.policy_asset)
        self.generate(node, 1)
        self.sync_all()
        assert_equal(node.getbalance()[r["asset"]], Decimal("150"))

        # The refusals the GUI mirrors, so a user is told before the node is.
        assert_raises_rpc_error(-8, "cannot be blinded", node.issueasset, 100, 1, True, None,
                                self.policy_asset, 8, None,
                                {"operationalkey": op_pub, "recoverykey": rec_pub})
        assert_raises_rpc_error(-8, "reissuance tokens", node.issueasset, 100, 0, False, None,
                                self.policy_asset, 8, None,
                                {"operationalkey": op_pub, "recoverykey": rec_pub})

if __name__ == '__main__':
    SupervisedAssetsTest().main()
