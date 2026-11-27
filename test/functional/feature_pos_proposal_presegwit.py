#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Before segwit is active, a compact proposal is rebuilt from the mempool and signed without a fetch.

Block assembly always adds a witness commitment, but the coinbase carries the commitment's nonce only once
segwit is active, and until then the rules check only that no transaction has a witness. A committee member
rebuilding a compact proposal must check what the rules check: before the fix it applied the commitment
check whatever the activation state, judged every proposal's witnesses wrong, and fetched each full proposal.

Three nodes each hold one of the three committee keys (quorum two), with segwit active only from height 1,000.
Blocks are made and certified, and no node ever has to fetch a full proposal.
"""

from test_framework.address import byte_to_base58
from test_framework.key import ECKey
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    return byte_to_base58(k.get_bytes() + b'\x01', 239), k.get_pubkey().get_bytes().hex()


class PosProposalPreSegwitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1", "-posvrf=1", "-posbls=1", "-poscommitteesize=3", "-posslotinterval=1",
            "-con_max_block_sig_size=4000", "-signblockscript=51", "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1", "-validatepegin=0", "-testactivationheight=segwit@1000",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [common + ["-posproducer", "-posproducerkey=%s" % self.stakers[i][0]] for i in range(3)]

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

    def run_test(self):
        self.wait_until(lambda: all(n.getblockcount() >= 6 for n in self.nodes), timeout=180)
        assert not self.nodes[0].getdeploymentinfo()["deployments"]["segwit"]["active"]
        h = min(n.getblockcount() for n in self.nodes) - 1
        assert_equal(len({n.getblockhash(h) for n in self.nodes}), 1)
        fetched = {i: sum(p["bytesrecv_per_msg"].get("posproposal", 0) for p in n.getpeerinfo())
                   for i, n in enumerate(self.nodes)}
        compact = {i: sum(p["bytesrecv_per_msg"].get("poscmpctprop", 0) for p in n.getpeerinfo())
                   for i, n in enumerate(self.nodes)}
        self.log.info("bytes of compact proposals received %s, of full proposals fetched %s", compact, fetched)
        assert all(v > 0 for v in compact.values()), compact
        assert_equal(sum(fetched.values()), 0)


if __name__ == '__main__':
    PosProposalPreSegwitTest().main()
