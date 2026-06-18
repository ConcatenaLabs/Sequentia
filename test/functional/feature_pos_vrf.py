#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests Proof-of-Stake with private VRF sortition (-posvrf).

In VRF mode each block's coinbase commits to the leader's VRF proof over the
slot seed (tagged OP_RETURN); validators verify the proof against the leader's
challenge key and enforce the proof-derived, stake-weighted time slot. Slots
are private: only a key holder can compute its own slot in advance.
See doc/sequentia/04-proof-of-stake.md §4.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

SLOT_INTERVAL = 2


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosVrfTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

        # A heavy and a light staker (small weights keep slots small).
        self.big_wif, self.big_pub = make_staker()
        self.small_wif, self.small_pub = make_staker()

        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=%d" % SLOT_INTERVAL,
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-staker=%s:8" % self.big_pub,
            "-staker=%s:2" % self.small_pub,
            "-validatepegin=0",
        ]
        self.extra_args = [list(common), list(common)]

    def run_test(self):
        n0, n1 = self.nodes[0], self.nodes[1]

        # The schedule RPC reports VRF mode and the total weight
        sched = n0.getposschedule()
        assert_equal(sched['sortition'], 'vrf')
        assert_equal(sched['total_weight'], 10)

        # --- A staker produces a VRF-certified block; the peer validates the
        # proof and accepts it. ---
        res = n0.generateposblock(self.big_wif)
        assert 'vrf_output' in res and len(res['vrf_output']) == 64
        assert 0 <= res['vrf_slot'] <= 10
        self.sync_blocks()
        assert_equal(n1.getblockcount(), 1)

        # The coinbase carries the tagged SEQVRF commitment
        block = n0.getblock(n0.getbestblockhash(), 2)
        coinbase_outs = block['tx'][0]['vout']
        tagged = [o for o in coinbase_outs
                  if o['scriptPubKey']['asm'].startswith('OP_RETURN')
                  and '534551565246' in o['scriptPubKey']['hex']]  # "SEQVRF"
        assert_equal(len(tagged), 1)

        # The block's time respects the proof-derived slot
        header = n0.getblockheader(n0.getbestblockhash())
        genesis = n0.getblockheader(n0.getblockhash(0))
        assert header['time'] >= genesis['time'] + res['vrf_slot'] * SLOT_INTERVAL

        # --- The slot is deterministic per (key, seed): producing from the
        # same staker at a new height yields a (potentially) different slot,
        # but repeated dry runs at the same height would yield the same. We
        # check determinism via vrfprove directly against the live seed. ---
        seed = n0.getposschedule()['seed']
        p1 = n0.vrfprove(self.small_wif, seed)
        p2 = n0.vrfprove(self.small_wif, seed)
        assert_equal(p1['output'], p2['output'])
        # And the peer can verify it without any secret
        v = n1.vrfverify(self.small_pub, seed, p1['proof'])
        assert_equal(v['valid'], True)
        assert_equal(v['output'], p1['output'])

        # --- Both stakers can produce; the chain advances and stays converged
        # across nodes (the peer fully validates each VRF proof). ---
        for i in range(8):
            wif = self.big_wif if i % 2 == 0 else self.small_wif
            r = n0.generateposblock(wif)
            assert 'vrf_output' in r
        assert_equal(n0.getblockcount(), 9)
        self.sync_blocks()
        assert_equal(n1.getbestblockhash(), n0.getbestblockhash())

        # --- A non-staker cannot produce ---
        outsider_wif, _ = make_staker()
        assert_raises_rpc_error(-5, "not a registered staker",
                                n0.generateposblock, outsider_wif)

        # --- A block without a VRF commitment is rejected by consensus:
        # getnewblockhex assembles without a proposer/proof, so its template
        # fails TestBlockValidity (bad-posvrf / challenge). ---
        assert_raises_rpc_error(-1, "TestBlockValidity failed",
                                n0.getnewblockhex)


if __name__ == '__main__':
    PosVrfTest().main()
