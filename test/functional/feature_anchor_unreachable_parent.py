#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A headless node with anchoring on and no reachable parent chain daemon must not start.

When con_bitcoin_anchor is set and the Bitcoin daemon cannot be reached,
AppInitMain asks whether to continue without following Bitcoin. sequentiad has
nobody to ask, so ThreadSafeQuestion answers "no" and startup aborts. The
alternative -- starting with validateanchor silently forced to 0 -- would leave
a node that cannot notice a Bitcoin reorganization invalidating its own chain,
and that serves that chain to every peer syncing from it.

This is what pins the "nobody to ask" half of the behaviour; the interactive
half lives in the GUI (BitcoinGUI::message, SEQ_ANCHOR_PROMPT).

See doc/sequentia/03-bitcoin-anchoring.md, "Starting without a Bitcoin node".
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch


class AnchorUnreachableParentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[]]

    def setup_network(self):
        # Deliberately not started: the whole test is about a node that must
        # refuse to come up.
        self.add_nodes(self.num_nodes, self.extra_args)

    def run_test(self):
        base_args = [
            "-validatepegin=0",
            "-initialfreecoins=10000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",  # OP_TRUE
            "-con_bitcoin_anchor=1",
        ]
        # Port 1 is privileged and never listening, so the mainchain RPC client
        # fails to connect rather than talking to something unexpected.
        unreachable_args = base_args + [
            "-validateanchor=1",
            "-mainchainrpchost=127.0.0.1",
            "-mainchainrpcport=1",
            "-mainchainrpcuser=nobody",
            "-mainchainrpcpassword=nothing",
        ]

        self.log.info("sequentiad refuses to start when the parent chain daemon is unreachable")
        self.nodes[0].assert_start_raises_init_error(
            extra_args=unreachable_args,
            expected_msg="could not reach the Bitcoin node it anchors its blocks to",
            match=ErrorMatch.PARTIAL_REGEX,
        )

        self.log.info("...and starts once anchor validation is explicitly waived")
        self.start_node(0, extra_args=base_args + ["-validateanchor=0"])
        status = self.nodes[0].getanchorstatus()
        assert status["validateanchor"] is False
        # Waived in the configuration, not at a prompt: no session warning, and
        # nothing has been taken on trust yet.
        assert status["unvalidatedbyprompt"] is False
        assert status["parentbackonline"] is False
        assert status["unverifiedescapingstalls"] == 0
        assert status["lastunverifiedescapingstall"] == -1
        self.stop_node(0)


if __name__ == "__main__":
    AnchorUnreachableParentTest().main()
