#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end distributed MuSig2 committee block production (-posaggcommittee).

Models a decentralized committee in which each member runs on its own node and
never shares its key. For each block:

  1. each member computes its VRF eligibility proof for the slot (vrfprove);
  2. the leader assembles the unsigned block and the hash to sign
     (getposblocktemplate), committing every member's eligibility proof;
  3. round 1 — each member produces a public nonce on its own node (musignonce);
  4. round 2 — each member produces a partial signature (musigpartialsign);
  5. the partials are aggregated into one 64-byte signature (musigaggregate)
     and checked (musigverify);
  6. the leader attaches its own signature and the aggregate and submits
     (submitposblock); every node validates and accepts it.

No node ever holds more than its own key, yet the committee certifies the block
with a single signature — the path to the paper's 100-member committees.
See doc/sequentia/04-proof-of-stake.md §6.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

COMMITTEE_SIZE = 3
QUORUM = 2  # strict majority of the expected committee size


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosDistributedCommitteeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True

        # Three unit-weight stakers; committee_size 3 ⇒ all are always selected.
        self.stakers = [make_staker() for _ in range(3)]
        common = [
            "-con_pos=1",
            "-posvrf=1",
            "-posaggcommittee=1",
            "-poscommitteesize=%d" % COMMITTEE_SIZE,
            "-posslotinterval=1",
            "-signblockscript=51",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-validatepegin=0",
        ] + ["-staker=%s:1" % pub for _, pub in self.stakers]
        self.extra_args = [list(common) for _ in range(3)]

    def produce_block(self, height):
        """Drive one distributed-committee block. Each member's signing RPCs are
        sent to a distinct node, so no node uses more than its own key."""
        leader_node = self.nodes[0]
        wifs = [w for w, _ in self.stakers]
        pubs = [p for _, p in self.stakers]
        # The node that "hosts" member i (member 0 = leader on node 0).
        member_node = [self.nodes[i] for i in range(3)]

        # Slot seed for the next block, in internal byte order (getposschedule
        # returns the reversed GetHex display form), and each member's VRF
        # eligibility proof over it.
        seed_display = leader_node.getposschedule()['seed']
        seed = bytes.fromhex(seed_display)[::-1].hex()
        proofs = [member_node[i].vrfprove(wifs[i], seed)['proof'] for i in range(3)]

        # Leader assembles the unsigned block + the hash to sign.
        tmpl = leader_node.getposblocktemplate(
            wifs[0],
            [{"pubkey": pubs[i], "vrfproof": proofs[i]} for i in range(3)])
        signhash = tmpl['signhash']
        members = tmpl['members']  # the signing set, as the node ordered it
        assert_equal(sorted(members), sorted(pubs))
        assert_equal(tmpl['height'], height)

        # Map each member pubkey to the node that holds its key.
        node_for = {pubs[i]: member_node[i] for i in range(3)}
        wif_for = {pubs[i]: wifs[i] for i in range(3)}
        sid = "blk%d" % height

        # Round 1: a public nonce from each member, on its own node.
        pubnonces = [node_for[pk].musignonce(sid, wif_for[pk], members, signhash)['pubnonce']
                     for pk in members]
        # Round 2: a partial signature from each member, on its own node.
        partials = [node_for[pk].musigpartialsign(sid, wif_for[pk], members, pubnonces, signhash)['partialsig']
                    for pk in members]

        # Aggregate (public) and sanity-check before submitting.
        sig = leader_node.musigaggregate(members, pubnonces, partials, signhash)['signature']
        assert_equal(len(sig), 128)  # 64 bytes hex
        assert leader_node.musigverify(members, signhash, sig)['valid']

        res = leader_node.submitposblock(tmpl['block'], wifs[0], sig)
        assert_equal(res['height'], height)
        return res['hash']

    def run_test(self):
        n0, n1, n2 = self.nodes

        # Registries agree across all three nodes.
        assert_equal(n0.getstakerinfo(), n1.getstakerinfo())
        assert_equal(n0.getstakerinfo(), n2.getstakerinfo())

        # A member that fails to produce a partial signature (single-use abuse)
        # is caught by the session store: reusing a session id is refused.
        seed = n0.getposschedule()['seed']
        wif0 = self.stakers[0][0]
        pubs = [p for _, p in self.stakers]
        n0.musignonce("dup", wif0, pubs, seed)
        try:
            n0.musignonce("dup", wif0, pubs, seed)
            raise AssertionError("expected duplicate-session rejection")
        except Exception as e:
            assert "already exists" in str(e), str(e)

        # A member listed twice cannot inflate the template's quorum count:
        # getposblocktemplate rejects duplicates up front (consensus would
        # reject the block anyway with bad-posvrf-member-duplicate).
        seed_internal = bytes.fromhex(seed)[::-1].hex()
        proof0 = n0.vrfprove(wif0, seed_internal)['proof']
        try:
            n0.getposblocktemplate(wif0, [{"pubkey": pubs[0], "vrfproof": proof0},
                                          {"pubkey": pubs[0], "vrfproof": proof0}])
            raise AssertionError("expected duplicate-member rejection")
        except Exception as e:
            assert "more than once" in str(e), str(e)

        # Produce several blocks via the full distributed flow; all nodes stay
        # converged on the committee-certified chain.
        for h in range(1, 6):
            block_hash = self.produce_block(h)
            self.sync_blocks()
            assert_equal(n0.getbestblockhash(), block_hash)
            assert_equal(n1.getbestblockhash(), block_hash)
            assert_equal(n2.getbestblockhash(), block_hash)
            # The accepted block is in aggregate-committee form (OP_1 challenge)
            # with one 64-byte aggregate signature, not per-member multisig.
            blk = n0.getblock(block_hash, 2)
            assert blk['signblock_challenge'].startswith('51')
            assert len(blk['signblock_witness_hex']) // 2 < 200

        assert_equal(n0.getblockcount(), 5)


if __name__ == '__main__':
    PosDistributedCommitteeTest().main()
