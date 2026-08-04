#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The one-time UTXO-set rewrite: Consensus::UtxoRecovery.

On 2026-07-29 a watchdog deleted the Sequentia testnet's founder treasury
wallet, and two of its outputs became unspendable for ever -- the private keys
and the wallet's master blinding key were both destroyed. The owner authorised
recovering them by consensus rule: at one agreed height, every node retires those
two outpoints from its UTXO set and adds two replacements. The real table lives
in CTestNetParams and is pinned by sequentia_chainparams_tests; what CANNOT be
pinned by a unit test is the machinery, because that only shows itself over a
chain that is built, reorged, and rebuilt.

So this drives the same code path over a custom chain, whose recovery table comes
from -con_utxo_recovery_* instead of being hard-coded. Nothing here is
recovery-specific except the table: ConnectBlock, DisconnectBlock and the undo
records are the ones the testnet will run.

What it proves, in order:
  1. one block early, nothing has happened;
  2. at the activation height the retired coins are gone and the created ones
     exist, with exactly the asset, amount and scriptPubKey asked for;
  3. disconnecting that block restores the UTXO set byte for byte, and
     reconnecting it re-applies the rewrite -- so a reorg neither loses it nor
     applies it twice;
  4. a -reindex from genesis reaches an identical UTXO set, which is the property
     most easily got wrong: the rewrite has to be part of connecting a block, not
     something done once to a live chainstate;
  5. a chain WITHOUT the table replays the very same blocks and does NOT rewrite
     anything -- the gate is load-bearing, and a fresh chain (regtest as shipped,
     a future testnet, mainnet) inherits nobody's accident;
  6. a recovered output then spends with an ordinary signature, with no special
     case anywhere in the spend path.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

# Recreated by the rewrite. A big policy-asset amount plus a token, mirroring the
# shape of the real table (398,000,000 tSEQ and the USDX reissuance token).
RECOVERED_POLICY_AMOUNT = 12345678
RECOVERED_TOKEN_AMOUNT = 1


def atoms(amount):
    return int(Decimal(str(amount)) * 100000000)


class UtxoRecoveryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # The chain starts with no recovery table at all: this is what a default
        # elementsregtest / regtest node is, and phase 5 comes back to it.
        # Node 1 stays untouched at genesis until phase 5b, where it syncs the
        # whole chain from node 0 for the first time.
        self.extra_args = [[], []]

    def setup_network(self):
        # Deliberately unconnected: node 1's value is that it has never seen a
        # block until it is asked to sync the finished chain from scratch.
        self.setup_nodes()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # -- helpers ------------------------------------------------------------

    def utxo_set_hash(self, node):
        return node.gettxoutsetinfo("hash_serialized_2")["hash_serialized_2"]

    def unspent_of(self, node, asset):
        """One listunspent entry holding `asset`, largest first."""
        utxos = sorted([u for u in node.listunspent() if u["asset"] == asset],
                       key=lambda u: u["amount"], reverse=True)
        assert utxos, "no unspent output of asset %s" % asset
        return utxos[0]

    def assert_present(self, node, outpoint, present, why):
        txid, vout = outpoint
        got = node.gettxout(txid, vout)
        if present:
            assert got is not None, "%s: %s:%d should be in the UTXO set" % (why, txid, vout)
        else:
            assert got is None, "%s: %s:%d should NOT be in the UTXO set" % (why, txid, vout)
        return got

    def created_outputs(self, node):
        """The rewrite's created coins, found the way an operator would.

        They are in no block, so no wallet rescan can see them and
        getrawtransaction does not know their transaction. scantxoutset reads the
        UTXO set itself, which is exactly where they live.
        """
        found = node.scantxoutset("start", ["addr(%s)" % self.treasury])["unspents"]
        return sorted(found, key=lambda u: u["vout"])

    # -- the test -----------------------------------------------------------

    def run_test(self):
        node = self.nodes[0]
        self.generate(node, 101, sync_fun=self.no_op)
        policy = node.dumpassetlabels()["bitcoin"]

        # An issued asset gives us a reissuance token to strand and recreate, so
        # the test covers a non-policy asset as well as the policy one.
        issued = node.issueasset(100, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        token = issued["token"]

        # The two outputs the "accident" strands. Ordinary outputs: what makes
        # them unrecoverable on the real chain is that their keys are gone, which
        # is not something a test can arrange -- and does not need to, because
        # the rewrite never looks at keys.
        doomed_policy = self.unspent_of(node, policy)
        doomed_token = self.unspent_of(node, token)
        retire = [(doomed_policy["txid"], doomed_policy["vout"]),
                  (doomed_token["txid"], doomed_token["vout"])]

        # The replacement wallet. Unconfidential, as on Sequentia, whose default
        # addresses are explicit bech32 -- and as the real table's P2WPKH is.
        self.treasury = node.getaddressinfo(node.getnewaddress())["unconfidential"]
        treasury_spk = node.getaddressinfo(self.treasury)["scriptPubKey"]

        activation = node.getblockcount() + 5
        recovery_args = [
            "-con_utxo_recovery_height=%d" % activation,
        ] + [
            "-con_utxo_recovery_retire=%s:%d" % (txid, vout) for txid, vout in retire
        ] + [
            "-con_utxo_recovery_create=%s:%d:%s" % (policy, atoms(RECOVERED_POLICY_AMOUNT), treasury_spk),
            "-con_utxo_recovery_create=%s:%d:%s" % (token, atoms(RECOVERED_TOKEN_AMOUNT), treasury_spk),
        ]
        self.log.info("activation height %d, retiring %s" % (activation, retire))
        self.restart_node(0, extra_args=recovery_args)

        # -- 1. one block early, nothing has happened -----------------------
        self.generate(node, activation - 1 - node.getblockcount(), sync_fun=self.no_op)
        assert_equal(node.getblockcount(), activation - 1)
        for outpoint in retire:
            self.assert_present(node, outpoint, True, "one block before activation")
        assert_equal(self.created_outputs(node), [])
        before = self.utxo_set_hash(node)

        # -- 2. the activation block rewrites the UTXO set -------------------
        activation_hash = self.generate(node, 1, sync_fun=self.no_op)[0]
        assert_equal(node.getblockcount(), activation)

        for outpoint in retire:
            self.assert_present(node, outpoint, False, "at activation")

        created = self.created_outputs(node)
        assert_equal(len(created), 2)
        # Both created coins come from one synthetic transaction, so they share a
        # txid and take vout 0 and 1 in table order.
        recovery_txid = created[0]["txid"]
        assert_equal(created[1]["txid"], recovery_txid)
        assert_equal([c["vout"] for c in created], [0, 1])
        assert_equal(created[0]["asset"], policy)
        assert_equal(created[0]["amount"], Decimal(str(RECOVERED_POLICY_AMOUNT)))
        assert_equal(created[1]["asset"], token)
        assert_equal(created[1]["amount"], Decimal(str(RECOVERED_TOKEN_AMOUNT)))
        for c in created:
            assert_equal(c["scriptPubKey"], treasury_spk)

        # Consensus sees them as ordinary coins; the wallet does not see them at
        # all, because no block contains the transaction that "made" them. That
        # is a real operational consequence of a UTXO rewrite, not an oversight,
        # and it is pinned here so nobody discovers it by surprise.
        for vout in (0, 1):
            self.assert_present(node, (recovery_txid, vout), True, "at activation")
        assert_equal([u for u in node.listunspent() if u["txid"] == recovery_txid], [])

        after = self.utxo_set_hash(node)
        assert after != before, "the rewrite did not change the UTXO set"

        # -- 3. disconnect undoes it exactly; reconnect re-applies it --------
        # A reorg must neither strand the recovery nor apply it twice. The undo
        # record has to restore the retired coins byte for byte -- one of the
        # real ones is confidential, so nothing else could.
        node.invalidateblock(activation_hash)
        assert_equal(node.getblockcount(), activation - 1)
        for outpoint in retire:
            self.assert_present(node, outpoint, True, "after disconnecting the activation block")
        for vout in (0, 1):
            self.assert_present(node, (recovery_txid, vout), False, "after disconnecting")
        assert_equal(self.utxo_set_hash(node), before)

        node.reconsiderblock(activation_hash)
        assert_equal(node.getbestblockhash(), activation_hash)
        assert_equal(self.utxo_set_hash(node), after)

        # Blocks after the activation block behave normally.
        self.generate(node, 3, sync_fun=self.no_op)
        settled_height = node.getblockcount()
        settled = self.utxo_set_hash(node)

        # -- 4a. the created coins survive a flush to disk --------------------
        # A plain restart reads the chainstate back out of leveldb rather than
        # replaying anything, so this is the one check that the rewritten coins
        # were really written down and not just held in the in-memory cache.
        self.restart_node(0, extra_args=recovery_args)
        assert_equal(self.utxo_set_hash(node), settled)
        assert_equal(len(self.created_outputs(node)), 2)

        # -- 4. a reindex from genesis reaches the identical UTXO set --------
        # The point of putting this in the block-connect path rather than doing it
        # once to a live chainstate: a node that has never seen the chain before,
        # or one rebuilding from the blocks on disk, must land on the same set.
        self.restart_node(0, extra_args=recovery_args + ["-reindex"])
        assert_equal(node.getblockcount(), settled_height)
        assert_equal(self.utxo_set_hash(node), settled)
        assert_equal(len(self.created_outputs(node)), 2)
        for outpoint in retire:
            self.assert_present(node, outpoint, False, "after reindex")

        # -- 5. without the table, the same blocks rewrite nothing ------------
        # This is the fresh-chain guarantee, tested rather than asserted: regtest
        # as shipped, a re-genesised testnet and mainnet all have an empty table,
        # and an empty table means the identical block history leaves the retired
        # coins alone. (The genesis-hash half of the gate is pinned by
        # sequentia_chainparams_tests, which cannot be reached from here without
        # a second chain.)
        self.restart_node(0, extra_args=["-reindex"])
        assert_equal(node.getblockcount(), settled_height)
        for outpoint in retire:
            self.assert_present(node, outpoint, True, "with no recovery table")
        assert_equal(self.created_outputs(node), [])
        assert self.utxo_set_hash(node) != settled, "the gate made no difference"

        # -- 5b. a node syncing from scratch converges on the same set --------
        # The reindex above replays local blocks; this is the real thing. Node 1
        # has been sitting at genesis all along and now downloads the entire
        # chain over P2P, applying the rewrite as it connects block `activation`
        # like any other block. A fresh sync landing anywhere else would mean the
        # chain could not be reproduced from its own history.
        self.restart_node(0, extra_args=recovery_args + ["-reindex"])
        fresh = self.nodes[1]
        assert_equal(fresh.getblockcount(), 0)
        self.restart_node(1, extra_args=recovery_args)
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes)
        assert_equal(fresh.getblockcount(), settled_height)
        assert_equal(self.utxo_set_hash(fresh), settled)
        for outpoint in retire:
            self.assert_present(fresh, outpoint, False, "on a node synced from scratch")
        for vout in (0, 1):
            self.assert_present(fresh, (recovery_txid, vout), True, "on a node synced from scratch")
        self.disconnect_nodes(0, 1)

        # -- 6. a recovered output spends with an ordinary signature ----------
        assert_equal(self.utxo_set_hash(node), settled)

        fee = Decimal("0.001")
        destination = node.getaddressinfo(node.getnewaddress())["unconfidential"]
        raw = node.createrawtransaction(
            [{"txid": recovery_txid, "vout": 0}],
            [{destination: Decimal(str(RECOVERED_POLICY_AMOUNT)) - fee, "asset": policy},
             {"fee": fee, "fee_asset": policy}])
        # No prevtxs and no special case: signrawtransactionwithwallet finds the
        # prevout in the UTXO set like any other coin, and the wallet holds the
        # key because the rewrite paid an ordinary P2WPKH it controls.
        signed = node.signrawtransactionwithwallet(raw)
        assert_equal(signed["complete"], True)
        spend_txid = node.sendrawtransaction(signed["hex"])
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.gettransaction(spend_txid)["confirmations"], 1)

        self.assert_present(node, (recovery_txid, 0), False, "after being spent")
        self.assert_present(node, (recovery_txid, 1), True, "the token was not spent")
        spent_to = [o for o in node.decoderawtransaction(signed["hex"])["vout"]
                    if o["scriptPubKey"].get("hex") == node.getaddressinfo(destination)["scriptPubKey"]]
        assert_equal(len(spent_to), 1)
        assert_equal(spent_to[0]["value"], Decimal(str(RECOVERED_POLICY_AMOUNT)) - fee)
        assert_equal(spent_to[0]["asset"], policy)
        # And the proceeds are ordinary wallet funds from here on, which is how
        # the treasury gets back to normal after the one manual sweep.
        assert_greater_than(
            sum(u["amount"] for u in node.listunspent() if u["asset"] == policy
                and u["txid"] == spend_txid), 0)


if __name__ == "__main__":
    UtxoRecoveryTest().main()
