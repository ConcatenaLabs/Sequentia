#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests the VRF primitive RPCs (vrfprove / vrfverify).

The VRF is the building block for private Proof-of-Stake sortition: a staker
can produce a unique, pseudorandom, verifiable output for a slot seed that
nobody else could have produced or predicted. See doc/sequentia/07-vrf.md.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.key import ECKey
from test_framework.address import byte_to_base58


def make_key():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class VrfTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[], []]

    def run_test(self):
        prover, verifier = self.nodes[0], self.nodes[1]
        wif, pub = make_key()
        alpha = "0011223344556677"

        # Prove on node0
        res = prover.vrfprove(wif, alpha)
        assert_equal(res['pubkey'], pub)
        proof = res['proof']
        output = res['output']
        assert_equal(len(proof), 2 * (33 + 32 + 32))  # gamma||c||s, hex
        assert_equal(len(output), 64)

        # Verify on node1 (no shared secret needed)
        v = verifier.vrfverify(pub, alpha, proof)
        assert_equal(v['valid'], True)
        assert_equal(v['output'], output)

        # Verification fails for the wrong input, wrong key, or tampered proof
        assert_equal(verifier.vrfverify(pub, "00112233445566ff", proof)['valid'], False)
        _, other_pub = make_key()
        assert_equal(verifier.vrfverify(other_pub, alpha, proof)['valid'], False)
        tampered = proof[:-2] + ('00' if proof[-2:] != '00' else '01')
        assert_equal(verifier.vrfverify(pub, alpha, tampered)['valid'], False)

        # Uniqueness/determinism: same (key, input) ⇒ same output; different
        # input ⇒ different output
        again = prover.vrfprove(wif, alpha)
        assert_equal(again['output'], output)
        other = prover.vrfprove(wif, "ffffffff")
        assert other['output'] != output
        assert_equal(verifier.vrfverify(pub, "ffffffff", other['proof'])['valid'], True)

        # A different key over the same input yields a different output, and
        # its proof does not verify under the first key
        wif2, pub2 = make_key()
        res2 = prover.vrfprove(wif2, alpha)
        assert res2['output'] != output
        assert_equal(verifier.vrfverify(pub, alpha, res2['proof'])['valid'], False)
        assert_equal(verifier.vrfverify(pub2, alpha, res2['proof'])['valid'], True)

        # Bad private key is rejected
        assert_raises_rpc_error(-5, "Invalid private key", prover.vrfprove, "notakey", alpha)


if __name__ == '__main__':
    VrfTest().main()
