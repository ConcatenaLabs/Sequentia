#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The reference-price feed is display only: it never writes the fee whitelist.

The whitelist has one source, the operator: a file written by hand, or a price
server pushing through setfeeexchangerates. The node also fetches market prices
from -referencepricesurl so a wallet can show values in a reference currency,
and those prices must stay out of the whitelist. Otherwise an asset the operator
left out is accepted anyway, clearing the table stops nothing, and a price
server's admission rules are not the node's policy.
"""

import http.server
import json
import threading
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

GOLD = 'aa' * 32
PRICES = {'SEQ': 2.0, 'GOLD': 3.0}
SEED_RATE = 100000000


class FeedHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps(PRICES).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class AnyAssetFeeFeedDisplayOnlyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.feed = http.server.HTTPServer(('127.0.0.1', 0), FeedHandler)
        threading.Thread(target=self.feed.serve_forever, daemon=True).start()
        self.extra_args = [[
            '-con_any_asset_fees=1',
            '-assetdir=%s:GOLD' % GOLD,
            '-referencepricesurl=http://127.0.0.1:%d/prices' % self.feed.server_address[1],
            '-referencepricespoll=1',
        ]]

    def let_the_feed_poll(self):
        # Long enough for several polls, and for any timer that might act on them.
        time.sleep(12)

    def run_test(self):
        node = self.nodes[0]
        seed = node.getfeeexchangerates()
        assert_equal(list(seed.values()), [SEED_RATE])
        policy = list(seed)[0]

        self.log.info("The node fetches the prices...")
        self.wait_until(lambda: node.getreferenceprices() == {'GOLD': 3, 'SEQ': 2})

        self.log.info("...and leaves the whitelist as it was")
        self.let_the_feed_poll()
        assert_equal(node.getfeeexchangerates(), seed)
        info = node.getfeeassetinfo(GOLD)[GOLD]
        assert_equal(info['market_price'], 3)
        assert_equal(info['listed'], False)
        assert_equal(info['accepted'], False)

        self.log.info("An asset the operator leaves out stays out")
        node.setfeeexchangerates({policy: 500000000}, False)
        self.let_the_feed_poll()
        assert_equal(node.getfeeexchangerates(), {policy: 500000000})

        self.log.info("A cleared whitelist stays cleared")
        node.setfeeexchangerates({}, False)
        self.let_the_feed_poll()
        assert_equal(node.getfeeexchangerates(), {})

        self.feed.shutdown()


if __name__ == '__main__':
    AnyAssetFeeFeedDisplayOnlyTest().main()
