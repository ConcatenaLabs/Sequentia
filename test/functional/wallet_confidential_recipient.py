#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A send to a confidential address is blinded, whatever else the transaction holds.

Sequentia is transparent by default and confidentiality is the holder's choice,
made per transfer by sending to a confidential (blech32) address. A lone blinded
output cannot be blinded when no input is blinded, so a send from explicit coins
needs a second blinded output beside the recipient:

- with change, the change is blinded too, and it is not given to the fee when
  that would leave the recipient alone: it stays, or becomes a zero-value
  blinded OP_RETURN when it is dust;
- with no change (a whole-balance send with the fee subtracted), the wallet adds
  a zero-value blinded OP_RETURN.

The recipient's output used to go out explicit in both cases, with no error.

On the raw path a blinding the caller asked for is dropped only when the caller
passes ignoreblindfail=true. Left unset, fundrawtransaction keeps a lone
confidential output blindable (as sendtoaddress does), and so does false, which
asks for exactly that; blindrawtransaction fails rather than return such an
output explicit, unset or false.
"""

from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

FEE_ASSET = 'bitcoin'
LONE_OUTPUT = "Unable to blind transaction: a confidential output cannot be blinded on its own when no input is blinded."


class WalletConfidentialRecipientTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-con_default_blinded_addresses=0", "-blindedaddresses=0",
                            "-con_blocksubsidy=5000000000", "-validatepegin=0", "-txindex=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def explicit_coin(self, wallet, amount):
        """Send `amount` to `wallet` as one explicit coin; return it."""
        addr = wallet.getaddressinfo(wallet.getnewaddress())['unconfidential']
        txid = self.miner.sendtoaddress(address=addr, amount=amount, fee_asset_label=FEE_ASSET)
        self.generatetoaddress(self.node, 1, self.miner_addr, sync_fun=self.no_op)
        coin = [u for u in wallet.listunspent() if u['txid'] == txid and u['amount'] == amount]
        assert_equal(len(coin), 1)
        assert_equal(coin[0]['amountblinder'], '00' * 32)
        return coin[0]

    def confirm(self, txid):
        self.generatetoaddress(self.node, 1, self.miner_addr, sync_fun=self.no_op)
        tx = self.node.getrawtransaction(txid, True)
        assert_equal(tx['confirmations'], 1)
        return tx

    def recipient_output(self, tx, conf_addr):
        unconf = self.receiver.getaddressinfo(conf_addr)['unconfidential']
        out = [v for v in tx['vout'] if v['scriptPubKey'].get('address') == unconf]
        assert_equal(len(out), 1)
        return out[0]

    def assert_received(self, txid, conf_addr, amount):
        """The receiver unblinds the payment and is credited the amount sent."""
        assert_equal(self.receiver.gettransaction(txid)['amount'][FEE_ASSET], amount)

    def run_test(self):
        self.node = self.nodes[0]
        self.miner = self.node.get_wallet_rpc(self.default_wallet_name)
        self.miner_addr = self.miner.getnewaddress()
        self.generatetoaddress(self.node, COINBASE_MATURITY + 10, self.miner_addr, sync_fun=self.no_op)
        self.node.createwallet("receiver")
        self.receiver = self.node.get_wallet_rpc("receiver")

        for ignore in (None, True, False):
            self.log.info("Whole balance, fee subtracted, no change (ignoreblindfail=%s): blinded with a dummy" % ignore)
            name = "sweep_%s" % ignore
            self.node.createwallet(name)
            w = self.node.get_wallet_rpc(name)
            self.explicit_coin(w, Decimal('5'))
            conf = self.receiver.getnewaddress("", "blech32")
            args = dict(address=conf, amount=5, subtractfeefromamount=True)
            if ignore is not None:
                args['ignoreblindfail'] = ignore
            txid = w.sendtoaddress(**args)
            tx = self.confirm(txid)
            out = self.recipient_output(tx, conf)
            assert 'valuecommitment' in out, out
            dummies = [v for v in tx['vout'] if v['scriptPubKey']['hex'] == '6a']
            assert_equal(len(dummies), 1)
            assert 'valuecommitment' in dummies[0]
            fee = sum(v['value'] for v in tx['vout'] if v['scriptPubKey']['type'] == 'fee')
            assert_equal(w.getbalances()['mine']['trusted'].get(FEE_ASSET, 0), 0)
            self.assert_received(txid, conf, Decimal('5') - fee)

        self.log.info("With change: the recipient and the change are blinded")
        self.node.createwallet("change")
        w = self.node.get_wallet_rpc("change")
        self.explicit_coin(w, Decimal('5'))
        conf = self.receiver.getnewaddress("", "blech32")
        txid = w.sendtoaddress(address=conf, amount=1, fee_asset_label=FEE_ASSET)
        tx = self.confirm(txid)
        assert 'valuecommitment' in self.recipient_output(tx, conf)
        mine = [v for v in tx['vout'] if v['scriptPubKey'].get('address') and
                w.getaddressinfo(v['scriptPubKey']['address'])['ismine']]
        assert_equal(len(mine), 1)
        assert 'valuecommitment' in mine[0]
        self.assert_received(txid, conf, Decimal('1'))

        self.log.info("Small leftovers, where the change used to go to the fee: blinded, or refused for funds")
        # Blinding the recipient needs a second blinded output, which costs
        # fee. A leftover below that cost cannot pay for the confidential send
        # the holder asked for: it is refused for funds rather than sent in the
        # clear, as it used to be (with the change given to the fee).
        refused, blinded_fees = [], []
        for leftover in ('0.00002', '0.00003', '0.00004', '0.00006', '0.0001', '0.0005', '0.002'):
            name = "leftover_%s" % leftover
            self.node.createwallet(name)
            w = self.node.get_wallet_rpc(name)
            self.explicit_coin(w, Decimal('1'))
            conf = self.receiver.getnewaddress("", "blech32")
            amount = Decimal('1') - Decimal(leftover)
            try:
                txid = w.sendtoaddress(address=conf, amount=amount, fee_asset_label=FEE_ASSET, fee_rate=1)
            except JSONRPCException as e:
                assert_equal(e.error['message'], "Insufficient funds")
                self.log.info("  leftover %s: refused, %s", leftover, e.error['message'])
                refused.append(Decimal(leftover))
                continue
            tx = self.confirm(txid)
            assert 'valuecommitment' in self.recipient_output(tx, conf), (leftover, tx['vout'])
            blinded = [v for v in tx['vout'] if 'valuecommitment' in v]
            fee = sum(v['value'] for v in tx['vout'] if v['scriptPubKey']['type'] == 'fee')
            self.log.info("  leftover %s: %d outputs, %d blinded, %d vbytes, fee %s", leftover, len(tx['vout']),
                          len(blinded), tx['vsize'], fee)
            assert_equal(len(blinded), 2)
            blinded_fees.append(fee)
            self.assert_received(txid, conf, amount)
        # Only leftovers smaller than any blinded send's fee are refused.
        assert blinded_fees
        assert all(r < min(blinded_fees) for r in refused), (refused, blinded_fees)

        self.log.info("Raw path: a lone confidential output is returned explicit only when the caller says so")
        self.node.createwallet("raw")
        w = self.node.get_wallet_rpc("raw")
        coin = self.explicit_coin(w, Decimal('5'))
        conf = self.receiver.getnewaddress("", "blech32")
        change = w.getaddressinfo(w.getnewaddress())['unconfidential']
        raw = w.createrawtransaction([{'txid': coin['txid'], 'vout': coin['vout']}],
                                     [{conf: 1}, {change: Decimal('3.999')}, {'fee': Decimal('0.001')}])
        assert_raises_rpc_error(-8, LONE_OUTPUT, w.blindrawtransaction, raw)
        assert_raises_rpc_error(-8, LONE_OUTPUT, w.blindrawtransaction, raw, False)
        explicit = w.decoderawtransaction(w.blindrawtransaction(raw, True))
        out = [v for v in explicit['vout'] if v['scriptPubKey'].get('address') ==
               self.receiver.getaddressinfo(conf)['unconfidential']][0]
        assert 'valuecommitment' not in out and not out.get('commitmentnonce_fully_valid'), out

        self.log.info("Raw path: fundrawtransaction keeps a lone confidential output blindable unless told otherwise")
        for ignore in (None, True, False):
            name = "fundraw_%s" % ignore
            self.node.createwallet(name)
            w = self.node.get_wallet_rpc(name)
            self.explicit_coin(w, Decimal('5'))
            conf = self.receiver.getnewaddress("", "blech32")
            unconf = self.receiver.getaddressinfo(conf)['unconfidential']
            raw = w.createrawtransaction([], [{conf: 5}])
            options = {'subtractFeeFromOutputs': [0]}
            if ignore is not None:
                options['ignoreblindfail'] = ignore
            funded = w.fundrawtransaction(raw, options)
            if ignore is True:
                # The caller chose: the output goes explicit, and is told so.
                assert "left unblinded" in funded['warnings'][0], funded.get('warnings')
                out = [v for v in w.decoderawtransaction(funded['hex'])['vout'] if v['scriptPubKey'].get('address') == unconf][0]
                assert not out.get('commitmentnonce_fully_valid'), out
                continue
            # Unset or false: kept blindable, and blinded by blindrawtransaction.
            assert 'warnings' not in funded, funded.get('warnings')
            blinded = w.decoderawtransaction(w.blindrawtransaction(funded['hex']))
            out = [v for v in blinded['vout'] if v['scriptPubKey'].get('address') == unconf][0]
            assert 'valuecommitment' in out, out
            signed = w.signrawtransactionwithwallet(w.blindrawtransaction(funded['hex']))
            assert_equal(self.node.testmempoolaccept([signed['hex']])[0]['allowed'], True)

if __name__ == '__main__':
    WalletConfidentialRecipientTest().main()
