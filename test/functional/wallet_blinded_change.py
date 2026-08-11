#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Blinded change when the wallet holds no blinded coins.

BlindTransaction refuses to blind a lone output when no input is blinded: with a
single output to blind the final blinding factor is forced to -vr, which opens the
commitment to anyone. The wallet used to answer that by quietly stripping the
blinding from the change output, which is self-perpetuating -- the demoted change
comes back as an explicit UTXO, so the next spend is in the same position and its
change is demoted too.

The constraint is on the *number* of blinded outputs, so it is escapable: give the
transaction a second blindable output (a zero-value blinded OP_RETURN, the same
device the wallet already uses for the mirror case of blinded inputs with nothing
to blind) and the lone blinded change becomes legal.

Sequentia is transparent by default, so the wallet only does this when confidential
change was actually asked for: a confidential change address, or a wallet handing
out confidential addresses (-blindedaddresses=1). With no request the change stays
explicit -- but no longer silently: the funding RPCs report it in `warnings`, and
`ignoreblindfail=false` turns it into an error.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import tx_from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

FEE_ASSET = 'bitcoin'


def is_blinded_nonce(vout):
    """True when the output carries a real blinding pubkey (blinded, or asking to be)."""
    return vout.get('commitmentnonce_fully_valid', False)


def op_return_dummies(decoded):
    """The zero-value blinded OP_RETURN outputs added purely to reach a blindable shape."""
    out = []
    for vout in decoded['vout']:
        asm = vout['scriptPubKey'].get('asm', '')
        if asm != 'OP_RETURN':
            continue
        if not is_blinded_nonce(vout):
            continue
        out.append(vout)
    return out


class WalletBlindedChangeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # The framework writes blindedaddresses=0 into every node's config, which is
        # also the Sequentia chain default: confidentiality is opt-in.
        self.extra_args = [[
            "-con_blocksubsidy=5000000000",
            "-validatepegin=0",
            "-txindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fund(self, node, to_address, amount, **options):
        """createrawtransaction + fundrawtransaction, returning (result, decoded tx)."""
        raw = node.createrawtransaction([], [{to_address: amount}])
        options.setdefault('fee_asset', FEE_ASSET)
        res = node.fundrawtransaction(raw, options)
        return res, node.decoderawtransaction(res['hex'])

    def run_test(self):
        node = self.nodes[0]

        # A wallet holding nothing but explicit coinbase outputs: exactly the state in
        # which a lone blinded change output cannot be blinded.
        explicit_addr = node.getnewaddress()
        assert_equal(node.getaddressinfo(explicit_addr)['confidential_key'], '')
        self.generatetoaddress(node, COINBASE_MATURITY + 1, explicit_addr, sync_fun=self.no_op)

        for utxo in node.listunspent():
            assert_equal(utxo['amountblinder'], '00' * 32)

        self.test_no_request_stays_explicit_but_loud(node, explicit_addr)
        self.test_strict_mode_errors(node, explicit_addr)
        self.test_confidential_change_address_is_honoured(node, explicit_addr)
        self.test_change_dropped_after_dummy_added(node, explicit_addr)
        self.test_blinded_by_default_wallet(node)
        self.test_mirror_case_still_works(node)

    def test_no_request_stays_explicit_but_loud(self, node, explicit_addr):
        self.log.info("No request for confidential change: change stays explicit, and says so")

        res, decoded = self.fund(node, explicit_addr, 1.0)
        change_pos = res['changepos']
        assert_greater_than(change_pos, -1)

        change = decoded['vout'][change_pos]
        assert not is_blinded_nonce(change), "change was blinded without being asked for"
        # The transparent default costs nothing extra: no dummy output is manufactured.
        assert_equal(op_return_dummies(decoded), [])

        assert 'warnings' in res, "the demotion was silent"
        assert_equal(len(res['warnings']), 1)
        assert "left unblinded" in res['warnings'][0], res['warnings']
        assert f"index {change_pos}" in res['warnings'][0], res['warnings']

        # walletcreatefundedpsbt reports it the same way.
        raw = node.createrawtransaction([], [{explicit_addr: 1.0}])
        psbt_res = node.walletcreatefundedpsbt(
            [], [{explicit_addr: 1.0}], 0, {'fee_asset': FEE_ASSET})
        assert 'warnings' in psbt_res, "walletcreatefundedpsbt demoted change silently"
        assert "left unblinded" in psbt_res['warnings'][0], psbt_res['warnings']
        assert raw  # createrawtransaction still works for the funding calls below

    def test_strict_mode_errors(self, node, explicit_addr):
        self.log.info("ignoreblindfail=false refuses to hand back a demoted change output")

        raw = node.createrawtransaction([], [{explicit_addr: 1.0}])
        assert_raises_rpc_error(
            -4,
            "Change output could not be blinded as there are no blinded inputs and no other blinded outputs.",
            node.fundrawtransaction, raw,
            {'fee_asset': FEE_ASSET, 'ignoreblindfail': False})

        assert_raises_rpc_error(
            -4,
            "Change output could not be blinded as there are no blinded inputs and no other blinded outputs.",
            node.walletcreatefundedpsbt, [], [{explicit_addr: 1.0}], 0,
            {'fee_asset': FEE_ASSET, 'ignoreblindfail': False})

        # The default is unchanged: the same call without the option still succeeds.
        assert node.fundrawtransaction(raw, {'fee_asset': FEE_ASSET})['hex']

    def test_confidential_change_address_is_honoured(self, node, explicit_addr):
        self.log.info("A confidential change address is honoured instead of demoted")

        conf_change = node.getnewaddress("", "blech32")
        assert node.getaddressinfo(conf_change)['confidential_key'] != ''

        plain_res, plain_decoded = self.fund(node, explicit_addr, 1.0)
        res, decoded = self.fund(node, explicit_addr, 1.0, changeAddress=conf_change)

        change_pos = res['changepos']
        assert_greater_than(change_pos, -1)
        assert is_blinded_nonce(decoded['vout'][change_pos]), "the request was demoted again"
        assert 'warnings' not in res, res.get('warnings')

        # It is honoured by manufacturing exactly one extra blindable output.
        dummies = op_return_dummies(decoded)
        assert_equal(len(dummies), 1)
        assert_equal(dummies[0]['value'], Decimal('0'))

        self.log.info("  transparent tx vsize %d, confidential-change vsize %d, fee %s -> %s",
                      plain_decoded['vsize'], decoded['vsize'], plain_res['fee'], res['fee'])
        assert_greater_than(decoded['vsize'], plain_decoded['vsize'])
        assert_greater_than(res['fee'], plain_res['fee'])

        # And it really blinds: the funded shape survives blinding, signing and relay,
        # which is the whole point -- BlindTransaction would refuse the old shape.
        blinded = node.blindrawtransaction(res['hex'])
        blinded_decoded = node.decoderawtransaction(blinded)
        assert 'valuecommitment' in blinded_decoded['vout'][change_pos], blinded_decoded['vout'][change_pos]
        # Price the dummy on its own: its serialized output plus its witness proofs, at
        # the witness discount. This is what honouring the request costs over and above
        # the change output's own rangeproof, which confidentiality would cost anyway.
        blinded_tx = tx_from_hex(blinded)
        dummy_index = next(i for i, v in enumerate(blinded_decoded['vout'])
                           if v['scriptPubKey'].get('asm') == 'OP_RETURN')
        dummy_wit = blinded_tx.wit.vtxoutwit[dummy_index]
        dummy_vsize = (len(blinded_tx.vout[dummy_index].serialize())
                       + (len(dummy_wit.vchRangeproof) + len(dummy_wit.vchSurjectionproof) + 3) // 4)
        self.log.info("  blinded vsize %d, of which the dummy output costs %d vbytes",
                      blinded_decoded['vsize'], dummy_vsize)

        signed = node.signrawtransactionwithwallet(blinded)
        assert_equal(signed['complete'], True)
        txid = node.sendrawtransaction(signed['hex'])
        self.generatetoaddress(node, 1, node.getnewaddress(), sync_fun=self.no_op)

        onchain = node.getrawtransaction(txid, True)
        assert 'valuecommitment' in onchain['vout'][change_pos]
        assert_equal(len(op_return_dummies(onchain)), 1)

        # The change came home blinded, so the next spend has a blinded input and needs
        # no help at all: this is the cycle that used to be impossible to leave.
        blinded_utxos = [u for u in node.listunspent() if u['amountblinder'] != '00' * 32]
        assert_greater_than(len(blinded_utxos), 0)

    def test_change_dropped_after_dummy_added(self, node, explicit_addr):
        self.log.info("Change dropped to fees after the dummy was added retires the dummy too")

        conf_change = node.getnewaddress("", "blech32")
        # Spend one explicit coinbase output down to (almost) nothing, so that the change
        # the dummy was added for is dropped as dust/into the fee.
        fee_asset_hex = node.dumpassetlabels()[FEE_ASSET]
        utxo = max((u for u in node.listunspent()
                    if u['amountblinder'] == '00' * 32 and u['asset'] == fee_asset_hex),
                   key=lambda u: u['amount'])
        inputs = [{'txid': utxo['txid'], 'vout': utxo['vout']}]

        # Learn what a confidential-change transaction costs here, then leave the wallet
        # progressively more change until it is small enough to be dropped into the fee.
        probe = node.fundrawtransaction(
            node.createrawtransaction(inputs, [{explicit_addr: utxo['amount'] - Decimal('1')}]),
            {'fee_asset': FEE_ASSET, 'changeAddress': conf_change, 'add_inputs': False})
        fee = probe['fee']

        dropped = False
        for multiplier in ['1', '1.25', '1.5', '2', '3', '5', '8']:
            amount = utxo['amount'] - (fee * Decimal(multiplier)).quantize(Decimal('0.00000001'))
            raw = node.createrawtransaction(inputs, [{explicit_addr: amount}])
            try:
                res = node.fundrawtransaction(raw, {
                    'fee_asset': FEE_ASSET,
                    'changeAddress': conf_change,
                    'add_inputs': False,
                })
            except Exception as e:
                # Running out of room for the fee is a legitimate outcome for a delta
                # that is too small; anything else is a real failure.
                assert "Could not cover fee" in str(e) or "Insufficient funds" in str(e), str(e)
                continue
            if res['changepos'] == -1:
                dropped = True
                decoded = node.decoderawtransaction(res['hex'])
                # With no change left there is nothing for a dummy to pair with, and
                # leaving one behind would be the very shape blinding refuses.
                assert_equal(op_return_dummies(decoded), [])
                blinded = node.blindrawtransaction(res['hex'])
                signed = node.signrawtransactionwithwallet(blinded)
                assert_equal(signed['complete'], True)
                node.sendrawtransaction(signed['hex'])
                self.generatetoaddress(node, 1, node.getnewaddress(), sync_fun=self.no_op)
                break

        assert dropped, "no delta produced a dropped change output; test did not exercise the path"

    def test_blinded_by_default_wallet(self, node):
        self.log.info("-blindedaddresses=1 alone is NOT read as a request (deliberate boundary)")

        self.restart_node(0, extra_args=self.extra_args[0] + ["-blindedaddresses=1"])
        node = self.nodes[0]
        default = node.get_wallet_rpc(self.default_wallet_name)

        # A fresh wallet holding nothing but an explicit coin, so that the transaction
        # below cannot lean on a blinded input.
        node.createwallet('blinddefault')
        fresh = node.get_wallet_rpc('blinddefault')
        funded_addr = fresh.getaddressinfo(fresh.getnewaddress())['unconfidential']
        default.sendtoaddress(address=funded_addr, amount=5.0, fee_asset_label=FEE_ASSET)
        self.generatetoaddress(self.nodes[0], 1, default.getnewaddress(), sync_fun=self.no_op)

        utxos = fresh.listunspent()
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]['amountblinder'], '00' * 32)

        # Explicit coin in, explicit recipient out. Handing out confidential addresses by
        # default says nothing about what this transaction's change should cost, so the
        # transaction stays fully explicit: no blinded output, no manufactured dummy, no
        # extra ~1160 vbytes. Turning this into a request is a default worth deciding on
        # purpose; until then it must not happen by accident.
        recipient = default.getaddressinfo(default.getnewaddress())['unconfidential']
        txid = fresh.sendtoaddress(address=recipient, amount=1.0, fee_asset_label=FEE_ASSET)
        self.generatetoaddress(self.nodes[0], 1, default.getnewaddress(), sync_fun=self.no_op)

        onchain = default.getrawtransaction(txid, True)
        assert_equal([v for v in onchain['vout'] if 'valuecommitment' in v], [])
        assert_equal(op_return_dummies(onchain), [])
        assert_equal([u['amountblinder'] == '00' * 32 for u in fresh.listunspent()], [True])

        # Asking explicitly still works on such a wallet.
        raw = fresh.createrawtransaction([], [{recipient: 1.0}])
        res = fresh.fundrawtransaction(raw, {
            'fee_asset': FEE_ASSET,
            'changeAddress': fresh.getnewaddress("", "blech32"),
        })
        assert_equal(len(op_return_dummies(fresh.decoderawtransaction(res['hex']))), 1)

        fresh.unloadwallet()
        self.restart_node(0, extra_args=self.extra_args[0])

    def test_mirror_case_still_works(self, node):
        self.log.info("The mirror case (blinded inputs, nothing to blind) is unchanged")

        node = self.nodes[0]
        node.createwallet('spender')
        spender = node.get_wallet_rpc('spender')
        default = node.get_wallet_rpc(self.default_wallet_name)

        # Give the second wallet exactly one blinded coin.
        conf_addr = spender.getnewaddress("", "blech32")
        default.sendtoaddress(address=conf_addr, amount=2.0, fee_asset_label=FEE_ASSET)
        self.generatetoaddress(self.nodes[0], 1, default.getnewaddress(), sync_fun=self.no_op)

        utxos = spender.listunspent()
        assert_equal(len(utxos), 1)
        assert utxos[0]['amountblinder'] != '00' * 32

        # Spend all of it to an explicit address: one blinded input, and after the change
        # is subtracted away, nothing to blind. The wallet must add the OP_RETURN.
        recipient = default.getaddressinfo(default.getnewaddress())['unconfidential']
        # No fee_asset_label: subtracting the fee from the output already determines it.
        txid = spender.sendtoaddress(address=recipient, amount=utxos[0]['amount'],
                                     subtractfeefromamount=True)
        self.generatetoaddress(self.nodes[0], 1, default.getnewaddress(), sync_fun=self.no_op)

        onchain = default.getrawtransaction(txid, True)
        assert_equal(len(op_return_dummies(onchain)), 1)
        assert_equal(spender.getbalance().get(FEE_ASSET, 0), 0)


if __name__ == '__main__':
    WalletBlindedChangeTest().main()
