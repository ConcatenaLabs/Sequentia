#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Converting staking rewards to NATIVE Bitcoin: the two endings, both real.

A reward conversion whose target is Bitcoin is a cross-chain atomic swap, and a
swap has exactly two endings. Either the maker takes the asset -- which spends
our contract and puts the secret on the Sequentia chain, where we read it and
use it to take the maker's Bitcoin -- or the maker walks away, and the timelock
gives the asset back. Both endings are the wallet's alone to execute: nobody is
watching, and there is nothing to press.

Both are driven here, end to end, against a REAL parent chain: node0 runs in
Bitcoin mode (`-chain=regtest`, so `g_con_elementsmode` is false), which is what
makes the claim this test proves worth proving -- the claim transaction is built
with Bitcoin's serialization and signed with Bitcoin's BIP143 sighash, and the
parent either accepts it or does not.

What is stubbed is the negotiation, and only that: the test plays the maker,
locking real Bitcoin on the parent chain and writing the agreed swap into the
wallet's ledger. Everything past that point is the node's own code -- finding
the secret, building and signing the claim, broadcasting it to the parent, and
on the other path, reclaiming the asset when the maker never came back.

Two bugs are pinned here, both of which survived unit tests and a live
same-chain conversion:

- Every read of the parent chain went for the payload of a JSON-RPC reply
  without unwrapping the envelope around it, so it silently found nothing. The
  maker's lock could never be verified and the fee estimate always fell back to
  its default. A claim path that has never once run against a real parent chain
  is a claim path nobody should trust.
- The refund paid its fee in the policy asset while refunding a different one,
  which leaves the transaction unbalanced in two assets at once. Rewards are
  mostly NOT the policy asset -- that is the point of an open fee market -- so
  the refund was broken for very nearly every swap it would ever be asked to
  rescue. The refund here is of an issued asset, deliberately.
"""

import json
import os
from decimal import Decimal
from io import BytesIO

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal, assert_greater_than, get_auth_cookie, get_datadir_path,
    rpc_port, p2p_port, initialize_datadir,
)
from test_framework.key import ECKey
from test_framework.address import program_to_witness, script_to_p2sh
from test_framework.messages import CTransaction, sha256, hash256
from test_framework.script import (
    CScript, CScriptNum, LegacySignatureHash, SIGHASH_ALL,
    OP_IF, OP_ELSE, OP_ENDIF, OP_SIZE, OP_EQUALVERIFY, OP_SHA256,
    OP_CHECKSIG, OP_CHECKLOCKTIMEVERIFY, OP_DROP, OP_1,
)

# The Bitcoin leg has to be worth claiming. The wallet refuses a swap whose
# proceeds would not cover claiming them -- and with no fee history on a fresh
# regtest parent it assumes the ceiling, 50 sat/vB, so it wants more than twice
# 175 * 50. A tenth of a Bitcoin clears that by a wide margin and keeps the
# arithmetic in the assertions easy to follow.
BTC_LEG = Decimal("0.1")
CLAIM_VSIZE = 175            # must match xchainconvert.cpp
MAX_FEERATE = 50             # ditto: the ceiling assumed with no estimate


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    return k


def htlc_script(digest, claim_pub, refund_pub, locktime):
    """The redeem script both legs share -- see BuildHtlcRedeemScript."""
    return CScript([
        OP_IF,
        OP_SIZE, CScriptNum(32), OP_EQUALVERIFY,
        OP_SHA256, digest, OP_EQUALVERIFY,
        claim_pub, OP_CHECKSIG,
        OP_ELSE,
        CScriptNum(locktime), OP_CHECKLOCKTIMEVERIFY, OP_DROP,
        refund_pub, OP_CHECKSIG,
        OP_ENDIF,
    ])


def p2wsh_bcrt(script):
    """The parent chain is in Bitcoin mode, so its bech32 prefix is bcrt."""
    return program_to_witness(0, sha256(bytes(script)), hrp="bcrt")


class RewardXchainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_chain(self):
        """Two chains, and they are not the same kind of chain.

        The framework writes one chain name into every node's config; here node0
        has to be a BITCOIN-mode chain and node1 an Elements-mode one, which is
        the entire point -- a parent that merely resembled Bitcoin would accept
        a claim that real Bitcoin rejects, and prove nothing.
        """
        self.log.info("Initializing test directory " + self.options.tmpdir)
        initialize_datadir(self.options.tmpdir, 0, "regtest")
        initialize_datadir(self.options.tmpdir, 1, "elementsregtest")

    def setup_network(self, split=False):
        self.nodes = []

        # node0: the parent chain, in BITCOIN mode. Not an Elements chain
        # pretending to be Bitcoin -- the claim is serialized and signed as a
        # Bitcoin transaction, so a parent that accepts it has proved something.
        # -txindex because the wallet looks the maker's lock up by txid alone,
        # which a parent daemon can only answer if it indexed it. This is the
        # same requirement pegins already place on the parent.
        self.add_nodes(1, [["-port=%d" % p2p_port(0), "-rpcport=%d" % rpc_port(0),
                            "-txindex=1", "-fallbackfee=0.0002"]], chain=["regtest"])
        self.start_node(0)
        parent_dir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(parent_dir, "regtest")

        # node1: Sequentia. Transparent, because an HTLC leg is inspected by
        # value and asset and a blinded one cannot be. Any-asset fees on,
        # because a refund of an issued asset pays its fee in that asset.
        seq_args = [
            "-port=%d" % p2p_port(1), "-rpcport=%d" % rpc_port(1),
            "-validatepegin=0", "-anyonecanspendaremine=1", "-signblockscript=51",
            "-txindex=1",       # so the test can read back the transactions it asserts on
            "-con_blocksubsidy=5000000000", "-initialfreecoins=2100000000000000",
            "-con_connect_genesis_outputs=1",
            "-con_default_blinded_addresses=0", "-blindedaddresses=0",
            "-con_any_asset_fees=1",
            "-mainchainrpchost=127.0.0.1", "-mainchainrpcport=%d" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u, "-mainchainrpcpassword=%s" % rpc_p,
        ]
        self.add_nodes(1, [seq_args], chain=["elementsregtest"])
        self.start_node(1)

    # -- helpers ----------------------------------------------------------

    def lock_bitcoin(self, redeem, amount):
        """Play the maker: lock Bitcoin on the parent chain, and confirm it."""
        parent = self.nodes[0]
        addr = p2wsh_bcrt(redeem)
        txid = parent.sendtoaddress(addr, amount)
        raw = parent.getrawtransaction(txid, True)
        vout = None
        for o in raw["vout"]:
            if o["scriptPubKey"]["address"] == addr:
                vout = o["n"]
                amt = o["value"]
        assert vout is not None, "the parent did not pay the contract"
        self.generatetoaddress(parent, 2, parent.getnewaddress(), sync_fun=self.no_op)
        conf = parent.getrawtransaction(txid, True)
        return txid, vout, amt, conf["blockhash"]

    def fund_seq_leg(self, redeem, asset, amount):
        """Our side of the swap: the asset, locked in the same contract."""
        seq = self.nodes[1]
        addr = script_to_p2sh(redeem)
        txid = seq.sendtoaddress(address=addr, amount=amount, assetlabel=asset,
                                 fee_asset_label="bitcoin")
        raw = seq.getrawtransaction(txid, True)
        vout = None
        for o in raw["vout"]:
            if o["scriptPubKey"].get("address") == addr:
                vout = o["n"]
        assert vout is not None, "the asset leg was not paid to the contract"
        self.generatetoaddress(seq, 1, seq.getnewaddress(), sync_fun=self.no_op)
        return txid, vout

    def write_swap(self, swap):
        """Put the negotiated swap where the wallet keeps them."""
        wallet_dir = os.path.join(get_datadir_path(self.options.tmpdir, 1),
                                  "elementsregtest", "wallets", self.wallet_name)
        with open(os.path.join(wallet_dir, "xchainswaps.json"), "w", encoding="utf8") as f:
            json.dump([swap], f)

    def maker_takes_the_asset(self, redeem, txid, vout, asset, amount, preimage, claim_key):
        """The maker spends our contract with the secret, revealing it on chain.

        This is the only way the secret ever reaches us, and it is the whole
        reason the swap is safe: the maker cannot take the asset without
        publishing what we need to take the Bitcoin.
        """
        seq = self.nodes[1]
        fee = Decimal("0.00002000")
        to = seq.getnewaddress()
        raw = seq.createrawtransaction(
            [{"txid": txid, "vout": vout, "sequence": 0xfffffffe}],
            [{to: amount - fee, "asset": asset}, {"fee": fee, "fee_asset": asset}])

        tx = CTransaction()
        tx.deserialize(BytesIO(bytes.fromhex(raw)))
        tx.nVersion = 2
        sighash, err = LegacySignatureHash(redeem, tx, 0, SIGHASH_ALL)
        assert err is None, err
        sig = claim_key.sign_ecdsa(sighash) + bytes([SIGHASH_ALL])
        # The claim branch: <sig> <preimage> <1> <redeem>. The 1 chooses the
        # hashlock side of the OP_IF, and it has to be OP_1 rather than a
        # one-byte push of 0x01: a scriptSig is script, and script is held to
        # minimal pushes. (The Bitcoin leg puts the same 1 on a WITNESS stack,
        # where it is plain data and the rule does not apply -- which is why the
        # node's own claim spells it {0x01}.)
        tx.vin[0].scriptSig = CScript([sig, preimage, OP_1, bytes(redeem)])
        tx.rehash()
        sent = seq.sendrawtransaction(tx.serialize().hex())
        self.generatetoaddress(seq, 1, seq.getnewaddress(), sync_fun=self.no_op)
        return sent

    # -- the test ---------------------------------------------------------

    def run_test(self):
        parent, seq = self.nodes

        self.wallet_name = "staker"
        seq.createwallet(wallet_name=self.wallet_name, descriptors=False, blank=False)
        wallet = seq.get_wallet_rpc(self.wallet_name)
        self.policy_asset = seq.dumpassetlabels()["bitcoin"]
        parent.createwallet(wallet_name="maker")
        pw = parent.get_wallet_rpc("maker")

        # Money on both chains.
        self.generatetoaddress(parent, 101, pw.getnewaddress(), sync_fun=self.no_op)
        self.generatetoaddress(seq, 101, wallet.getnewaddress(), sync_fun=self.no_op)
        assert_greater_than(pw.getbalance(), BTC_LEG)

        # The rewards being converted are ISSUED assets, not the policy asset.
        # That is what a staking reward usually is on a chain where fees are
        # payable in anything, and it is the case the refund used to get wrong.
        #
        # Two of them, because a refund has two ways to pay for itself and both
        # have to work. `taken` is one this node accepts for fees, so a refund
        # of it needs nothing but itself. `untaken` is one it does not, so a
        # refund of it has to reach into the wallet for the fee -- the case that
        # would otherwise leave an asset stuck until somebody noticed.
        taken = wallet.issueasset(assetamount=1000, tokenamount=1, blind=False,
                                  fee_asset="bitcoin")["asset"]
        untaken = wallet.issueasset(assetamount=1000, tokenamount=1, blind=False,
                                    fee_asset="bitcoin")["asset"]
        self.generatetoaddress(seq, 1, wallet.getnewaddress(), sync_fun=self.no_op)

        rates = seq.getfeeexchangerates()
        if isinstance(rates, dict) and "rates" in rates:
            rates = rates["rates"]
        rates = {k: v for k, v in rates.items()}
        rates[taken] = 100000000
        seq.setfeeexchangerates(rates, False)
        assert taken in seq.getfeeexchangerates()
        assert untaken not in seq.getfeeexchangerates()
        self.log.info("fee-accepted asset %s..., unaccepted asset %s...",
                      taken[:12], untaken[:12])

        self.claim_ending(parent, seq, wallet, pw, taken)
        self.refund_ending(parent, seq, wallet, pw, taken)
        self.refund_ending(parent, seq, wallet, pw, untaken, fee_from_wallet=True)

    # ---------------------------------------------------------------------

    def claim_ending(self, parent, seq, wallet, pw, asset):
        """The maker takes the asset, so we take the Bitcoin."""
        self.log.info("ENDING ONE: the maker takes the asset and we claim the Bitcoin")

        preimage = os.urandom(32)
        digest = sha256(preimage)

        maker_seq_claim = make_key()      # the maker takes our asset with this
        taker_seq_refund = make_key()     # we take it back with this, later
        maker_btc_refund = make_key()     # the maker's way out of its own lock
        taker_btc_claim = make_key()      # we take the Bitcoin with this

        btc_locktime = parent.getblockcount() + 500
        btc_redeem = htlc_script(digest, taker_btc_claim.get_pubkey().get_bytes(),
                                 maker_btc_refund.get_pubkey().get_bytes(), btc_locktime)
        btc_txid, btc_vout, btc_amt, _ = self.lock_bitcoin(btc_redeem, BTC_LEG)
        btc_height = parent.getblockcount() - 1
        self.log.info("  the maker locked %s BTC at %s:%d", btc_amt, btc_txid[:16], btc_vout)

        seq_locktime = seq.getblockcount() + 200
        seq_redeem = htlc_script(digest, maker_seq_claim.get_pubkey().get_bytes(),
                                 taker_seq_refund.get_pubkey().get_bytes(), seq_locktime)
        leg = Decimal("100")
        seq_txid, seq_vout = self.fund_seq_leg(seq_redeem, asset, leg)
        self.log.info("  we locked %s of the asset at %s:%d", leg, seq_txid[:16], seq_vout)

        self.write_swap({
            "state": "seq_funded",
            "time": 1,
            "offer_id": "test-claim",
            "maker_pubkey": maker_seq_claim.get_pubkey().get_bytes().hex(),
            "asset": asset,
            "seq_amount": int(leg * 100000000),
            "btc_amount": int(BTC_LEG * 100000000),
            "hash_h": digest.hex(),
            "maker_seq_claim_pub": maker_seq_claim.get_pubkey().get_bytes().hex(),
            "maker_btc_refund_pub": maker_btc_refund.get_pubkey().get_bytes().hex(),
            "taker_seq_refund_pub": taker_seq_refund.get_pubkey().get_bytes().hex(),
            "taker_btc_claim_pub": taker_btc_claim.get_pubkey().get_bytes().hex(),
            "taker_seq_refund_priv": taker_seq_refund.get_bytes().hex(),
            "taker_btc_claim_priv": taker_btc_claim.get_bytes().hex(),
            "seq_locktime": seq_locktime,
            "btc_locktime": btc_locktime,
            "btc_leg_txid": btc_txid,
            "btc_leg_vout": btc_vout,
            "btc_leg_script": bytes(btc_redeem).hex(),
            "btc_leg_amount": int(BTC_LEG * 100000000),
            "btc_leg_height": btc_height,
            "seq_fund_txid": seq_txid,
            "seq_fund_vout": seq_vout,
            "seq_redeem": bytes(seq_redeem).hex(),
            "preimage": "",
            "btc_claim_txid": "",
            "error": "",
        })

        # Before the maker moves there is nothing to do, and the wallet must not
        # invent something: an unclaimed contract is not a stuck one.
        pending = wallet.resumerewardswaps()
        assert_equal(len(pending), 1)
        assert_equal(pending[0]["state"], "seq_funded")
        assert_equal(pending[0]["asset"], asset)
        assert "bitcoin_claim_txid" not in pending[0]
        # And a staker can see exactly what is locked and for how long.
        assert_equal(pending[0]["amount"], leg)
        assert_greater_than(pending[0]["our_refund"]["blocks_to_go"], 0)
        assert_greater_than(pending[0]["maker_refund"]["blocks_to_go"], 0)
        # The private keys that redeem both legs are NOT in the listing.
        for banned in ("taker_seq_refund_priv", "taker_btc_claim_priv", "preimage"):
            assert banned not in pending[0], "%s must not be exposed" % banned
        self.log.info("  nothing to do yet, and the listing keeps the keys to itself")

        # The maker takes the asset. The secret is now public.
        spend = self.maker_takes_the_asset(seq_redeem, seq_txid, seq_vout, asset,
                                           leg, preimage, maker_seq_claim)
        self.log.info("  the maker took the asset in %s, revealing the secret", spend[:16])

        # Now the wallet should read the secret off the chain and take the
        # Bitcoin, by itself, with nobody watching.
        left = wallet.resumerewardswaps()
        assert_equal(left, [])

        done = wallet.listrewardswaps(True)
        assert_equal(len(done), 1)
        assert_equal(done[0]["state"], "btc_claimed")
        claim_txid = done[0]["bitcoin_claim_txid"]
        self.log.info("  the wallet claimed the Bitcoin in %s", claim_txid[:16])

        # The parent chain is the judge of whether that claim was well formed.
        assert claim_txid in parent.getrawmempool(), "the parent did not accept the claim"
        self.generatetoaddress(parent, 1, pw.getnewaddress(), sync_fun=self.no_op)
        claim = parent.getrawtransaction(claim_txid, True)
        assert_equal(claim["confirmations"], 1)
        assert_equal(len(claim["vout"]), 1)

        # It paid what it should, to a bech32 address, having spent the contract
        # with the secret.
        expected = int(BTC_LEG * 100000000) - CLAIM_VSIZE * MAX_FEERATE
        assert_equal(int(claim["vout"][0]["value"] * 100000000), expected)
        assert_equal(claim["vout"][0]["scriptPubKey"]["type"], "witness_v0_keyhash")
        assert_equal(claim["vin"][0]["txid"], btc_txid)
        assert_equal(claim["vin"][0]["vout"], btc_vout)
        assert preimage.hex() in claim["vin"][0]["txinwitness"], \
            "the claim did not spend the hashlock branch"
        self.log.info("  the parent confirmed it: %d satoshis, hashlock branch, our address",
                      expected)

        # Running again must be a no-op rather than a second claim.
        assert_equal(wallet.resumerewardswaps(), [])
        assert_equal(len(wallet.listrewardswaps(True)), 1)

    # ---------------------------------------------------------------------

    def refund_ending(self, parent, seq, wallet, pw, asset, fee_from_wallet=False):
        """The maker never comes back, so the timelock gives the asset back."""
        self.log.info("ENDING TWO%s: the maker walks away and the timelock returns the asset",
                      " (fee from the wallet)" if fee_from_wallet else "")

        preimage = os.urandom(32)
        digest = sha256(preimage)
        maker_seq_claim = make_key()
        taker_seq_refund = make_key()
        maker_btc_refund = make_key()
        taker_btc_claim = make_key()

        btc_locktime = parent.getblockcount() + 500
        btc_redeem = htlc_script(digest, taker_btc_claim.get_pubkey().get_bytes(),
                                 maker_btc_refund.get_pubkey().get_bytes(), btc_locktime)
        btc_txid, btc_vout, _, _ = self.lock_bitcoin(btc_redeem, BTC_LEG)

        # A refund needs the locktime to have passed, so put it just ahead and
        # then mine past it.
        seq_locktime = seq.getblockcount() + 3
        seq_redeem = htlc_script(digest, maker_seq_claim.get_pubkey().get_bytes(),
                                 taker_seq_refund.get_pubkey().get_bytes(), seq_locktime)
        leg = Decimal("50")
        seq_txid, seq_vout = self.fund_seq_leg(seq_redeem, asset, leg)

        offer_id = "test-refund-wallet-fee" if fee_from_wallet else "test-refund"
        before = wallet.getbalance()[asset]

        self.write_swap({
            "state": "seq_funded",
            "time": 2,
            "offer_id": offer_id,
            "maker_pubkey": maker_seq_claim.get_pubkey().get_bytes().hex(),
            "asset": asset,
            "seq_amount": int(leg * 100000000),
            "btc_amount": int(BTC_LEG * 100000000),
            "hash_h": digest.hex(),
            "maker_seq_claim_pub": maker_seq_claim.get_pubkey().get_bytes().hex(),
            "maker_btc_refund_pub": maker_btc_refund.get_pubkey().get_bytes().hex(),
            "taker_seq_refund_pub": taker_seq_refund.get_pubkey().get_bytes().hex(),
            "taker_btc_claim_pub": taker_btc_claim.get_pubkey().get_bytes().hex(),
            "taker_seq_refund_priv": taker_seq_refund.get_bytes().hex(),
            "taker_btc_claim_priv": taker_btc_claim.get_bytes().hex(),
            "seq_locktime": seq_locktime,
            "btc_locktime": btc_locktime,
            "btc_leg_txid": btc_txid,
            "btc_leg_vout": btc_vout,
            "btc_leg_script": bytes(btc_redeem).hex(),
            "btc_leg_amount": int(BTC_LEG * 100000000),
            "btc_leg_height": parent.getblockcount() - 1,
            "seq_fund_txid": seq_txid,
            "seq_fund_vout": seq_vout,
            "seq_redeem": bytes(seq_redeem).hex(),
            "preimage": "",
            "btc_claim_txid": "",
            "error": "",
        })

        # Too early: the timelock has not passed, and a refund attempted before
        # it would simply be rejected. The wallet must wait rather than try.
        pending = wallet.resumerewardswaps()
        assert_equal(len(pending), 1)
        assert_equal(pending[0]["state"], "seq_funded")
        self.log.info("  before the timelock: still waiting, %d blocks to go",
                      pending[0]["our_refund"]["blocks_to_go"])

        self.generatetoaddress(seq, 5, wallet.getnewaddress(), sync_fun=self.no_op)

        left = wallet.resumerewardswaps()
        assert_equal(left, [])
        swaps = {s["offer_id"]: s for s in wallet.listrewardswaps(True)}
        assert_equal(swaps[offer_id]["state"], "refunded")
        refund_txid = swaps[offer_id]["refund_txid"]

        self.generatetoaddress(seq, 1, wallet.getnewaddress(), sync_fun=self.no_op)
        after = wallet.getbalance()[asset]

        # How much came back says which way the fee was paid, and both answers
        # have to be exactly right.
        #
        # When the node takes this asset for fees, the refund pays for itself
        # out of the refunded amount. Paying in the POLICY asset instead -- what
        # the code used to do -- leaves the transaction short by the fee in the
        # asset it holds and conjuring the fee in one it does not, so it is
        # rejected every time, which is a poor way to discover that the safety
        # net has a hole in it.
        #
        # When the node does not take this asset, the fee comes from the
        # wallet's own coins and the asset comes back WHOLE. That is the better
        # outcome of the two: it is the staker's asset, not the fee's.
        recovered = after - before

        # Which asset actually paid is read off the refund transaction rather
        # than inferred from balances -- the wallet is also collecting block
        # subsidies here, and a balance delta cannot tell those apart from a
        # fee. The fee output is the one with an empty scriptPubKey.
        rtx = seq.getrawtransaction(refund_txid, True)
        fee_outs = [o for o in rtx["vout"] if not o["scriptPubKey"]["hex"]]
        assert_equal(len(fee_outs), 1)
        fee_asset = fee_outs[0]["asset"]
        fee_paid = fee_outs[0]["value"]

        # The amount is checked as a property, not a constant: it is derived
        # from the transaction's MEASURED size at this node's relay rate,
        # converted into whichever asset pays. Pinning a number would only
        # record today's rate -- and a flat number is precisely what was wrong
        # before, since the same atom count is dust in one asset and an absurd
        # fee in another.
        assert_greater_than(fee_paid, 0)
        expected_rate_band = Decimal(rtx["vsize"]) / Decimal(100000000)   # ~1 atom/vB at the default
        assert_greater_than(fee_paid * 4, expected_rate_band)
        assert_greater_than(expected_rate_band * 4, fee_paid)

        if fee_from_wallet:
            # The asset comes home WHOLE, and the policy asset paid instead.
            assert_equal(recovered, leg)
            assert_equal(fee_asset, self.policy_asset)
            self.log.info("  the asset came home whole: %s, with %s of the policy asset paying",
                          recovered, fee_paid)
        else:
            # The asset paid for its own rescue, and nothing else was touched.
            assert_equal(fee_asset, asset)
            assert_equal(recovered, leg - fee_paid)
            self.log.info("  the asset came home: %s of %s, having paid %s of itself",
                          recovered, leg, fee_paid)


if __name__ == '__main__':
    RewardXchainTest().main()
