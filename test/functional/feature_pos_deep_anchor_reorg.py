#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A DEEP Bitcoin reorg must roll the Sequentia chain back to ANY depth.

The core anchoring invariant (doc/sequentia/03-bitcoin-anchoring.md §intro and
§3, doc/sequentia/04-proof-of-stake.md §6):

    A Sequentia block is valid IF AND ONLY IF its Bitcoin anchor
    (`m_anchor_hash`) is on Bitcoin's best chain. The chain reorganizes IF AND
    ONLY IF Bitcoin reorganizes away a referenced block — TO ANY DEPTH.

There is NO PoS/finality floor that keeps a block whose anchor was orphaned, and
NO "deeper parent reorg needs operator action" escape hatch: immediate finality,
the immediate-finality gate, and PoS checkpoints are ALWAYS modulo a Bitcoin
reorg. Finality stops a SEQ-internal competitor; it never overrides Bitcoin.
A genuine still-buried checkpoint is the long-range defense, but a checkpoint
whose OWN Bitcoin commitment is orphaned by the reorg must retreat with it.

This test has two scenarios, both the worst case for "to any depth": EVERY
Sequentia block (1..N) is anchored to the SAME Bitcoin block B (the parent is
kept still while producing) and every one is quorum-certified, hence immediately
final. A deep parent reorg then orphans B outright. The whole anchored chain
(1..N) must be invalidated and the tip must fall all the way back to genesis;
production rebuilds on the parent's new best chain; and a FRESH node syncs the
canonical chain. No node may remain stuck on the orphaned-anchor blocks.

  Scenario A (no floor): plain deep reorg, no checkpoints. Reproduces the
  live-testnet failure where blocks 1..6 all anchored to one testnet4 block that
  a reorg orphaned, yet the chain stayed stuck.

  Scenario B (checkpoint floor must retreat): a Sequentia block is checkpointed
  into the parent and buried, raising the checkpoint-finalized point ABOVE the
  reorg's eventual rollback depth. The SAME deep reorg then orphans both the
  checkpointed block's anchor AND the checkpoint's own parent-chain commitment.
  The chain must STILL roll all the way back: the finalized point retreats
  because its commitment left Bitcoin's best chain (doc 04 §6). With the buggy
  "stop at finalized_height / never lower the floor" behavior the chain stays
  stuck on a block whose anchor is off Bitcoin's best chain — the forbidden
  compromise.

Topology:
  node0: the parent chain (stands in for Bitcoin); holds a wallet.
  node1: the Sequentia chain, anchored to node0, single genesis-config staker so
         each `generateposblock` block is quorum(1)-certified -> immediately final.
  node2: a FRESH Sequentia node, started later, that must sync the canonical
         (rebuilt) chain and never the orphaned-anchor branch.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    get_auth_cookie,
    get_datadir_path,
    rpc_port,
    p2p_port,
)
from test_framework.key import ECKey
from test_framework.address import byte_to_base58

CHECKPOINT_DEPTH = 2  # parent confs a checkpoint commitment needs to finalize


def make_staker():
    k = ECKey()
    k.generate(compressed=True)
    wif = byte_to_base58(k.get_bytes() + b'\x01', 239)
    pub = k.get_pubkey().get_bytes().hex()
    return wif, pub


class PosDeepAnchorReorgTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.staker_wif, self.staker_pub = make_staker()

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self, split=False):
        self.nodes = []

        parent_chain = "elementsregtest"
        parent_args = [
            "-port=" + str(p2p_port(0)),
            "-rpcport=" + str(rpc_port(0)),
            "-validatepegin=0",
            "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-blindedaddresses=0",
            "-fallbackfee=0.0001",
            "-txindex=1",
        ]
        self.add_nodes(1, [parent_args], chain=[parent_chain])
        self.start_node(0)
        self.parentgenesis = self.nodes[0].getblockhash(0)

        datadir = get_datadir_path(self.options.tmpdir, 0)
        rpc_u, rpc_p = get_auth_cookie(datadir, parent_chain)
        self.pos_common = [
            "-validatepegin=0",
            "-initialfreecoins=0",
            "-con_blocksubsidy=5000000000",
            "-anyonecanspendaremine=1",
            "-signblockscript=51",
            "-blindedaddresses=0",
            "-con_pos=1",
            "-posvrf=1",
            "-posslotinterval=1",
            "-staker=%s:1" % self.staker_pub,
            "-con_bitcoin_anchor=1",
            "-validateanchor=1",
            "-anchorpollinterval=1",
            "-anchorminconf=1",
            "-poscheckpointdepth=%d" % CHECKPOINT_DEPTH,
            "-mainchainrpchost=127.0.0.1",
            "-mainchainrpcport=%s" % rpc_port(0),
            "-mainchainrpcuser=%s" % rpc_u,
            "-mainchainrpcpassword=%s" % rpc_p,
            "-parentgenesisblockhash=%s" % self.parentgenesis,
        ]
        seq_args = ["-port=" + str(p2p_port(1)), "-rpcport=" + str(rpc_port(1))] + self.pos_common
        self.add_nodes(1, [seq_args], chain=["elementsregtest"])
        self.start_node(1)

        # node2 is added but NOT started yet: it joins fresh, after the reorg,
        # to prove a brand-new node syncs the canonical chain.
        fresh_args = ["-port=" + str(p2p_port(2)), "-rpcport=" + str(rpc_port(2))] + self.pos_common
        self.add_nodes(1, [fresh_args], chain=["elementsregtest"])

        self.nodes[0].createwallet(wallet_name="w", descriptors=True)

    def wait_until_count(self, node, target, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if node.getblockcount() == target:
                return
            time.sleep(0.25)
        raise AssertionError("node stuck at height %d, expected %d within %ds"
                             % (node.getblockcount(), target, timeout))

    def produce_all_anchored_to_still_B(self, seq, parent, n):
        """Produce n SEQ blocks while the parent is still, asserting every block
        anchors to the same single parent block B. Returns (b_height, b_hash)."""
        b_height = parent.getblockcount()
        b_hash = parent.getbestblockhash()
        anchors = set()
        base = seq.getblockcount()
        for _ in range(n):
            seq.generateposblock(self.staker_wif)
            hdr = seq.getblockheader(seq.getbestblockhash())
            anchors.add((hdr['anchorheight'], hdr['anchorhash']))
        assert_equal(seq.getblockcount(), base + n)
        assert_equal(anchors, {(b_height, b_hash)})
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')
        return b_height, b_hash

    def deep_orphan_B(self, parent, b_hash, b_height, evict_txids=None):
        """Invalidate B on the parent and mine a strictly longer competing
        branch so B (and everything after it) is off the parent's best chain.

        Any txids in `evict_txids` (e.g. a checkpoint commitment that was mined
        at/after B) are double-spent (BIP125) on the new branch so they cannot
        re-confirm — modelling a commitment that is genuinely orphaned by the
        reorg rather than merely re-mined from the mempool."""
        parent.invalidateblock(b_hash)
        assert_equal(parent.getblockcount(), b_height - 1)
        for txid in (evict_txids or []):
            self.double_spend_inputs(parent, txid)
            assert txid not in parent.getrawmempool()
        self.generatetoaddress(parent, 4, parent.getnewaddress(), sync_fun=self.no_op)
        assert parent.getblockcount() > b_height
        assert_equal(parent.getblockheader(b_hash)['confirmations'], -1)

    def double_spend_inputs(self, parent, txid):
        """Spend txid's inputs back to a fresh address so txid can never confirm."""
        dec = parent.decoderawtransaction(parent.getrawtransaction(txid))
        inputs, in_total = [], 0
        for vin in dec['vin']:
            prev = parent.decoderawtransaction(parent.getrawtransaction(vin['txid']))
            in_total += prev['vout'][vin['vout']]['value']
            inputs.append({"txid": vin['txid'], "vout": vin['vout']})
        fee = 0.001
        raw = parent.createrawtransaction(inputs, [
            {parent.getnewaddress(): round(float(in_total) - fee, 8)},
            {"fee": fee},
        ])
        signed = parent.signrawtransactionwithwallet(raw)
        assert signed['complete']
        parent.sendrawtransaction(signed['hex'])

    def commit_checkpoint(self, parent, payload):
        # Mark the commitment replaceable (BIP125) so that, if the reorg returns
        # it to the mempool, a higher-fee double-spend can evict it — letting us
        # model a commitment that is genuinely orphaned, not re-mined.
        raw = parent.createrawtransaction([], [{"data": payload}])
        funded = parent.fundrawtransaction(raw, {"replaceable": True})['hex']
        signed = parent.signrawtransactionwithwallet(funded)['hex']
        return parent.sendrawtransaction(signed)

    def run_test(self):
        parent, seq, fresh = self.nodes

        # ============================================================
        # Scenario A — no floor: a plain deep reorg of the whole chain.
        # ============================================================
        addr = parent.getnewaddress()
        self.generatetoaddress(parent, 101, addr, sync_fun=self.no_op)
        b_height, b_hash = self.produce_all_anchored_to_still_B(seq, parent, 6)
        orphaned_tip_a = seq.getbestblockhash()
        orphaned_b1_a = seq.getblockhash(1)
        self.log.info("[A] produced 6 SEQ blocks all anchored to parent B=%s (h=%d)"
                      % (b_hash[:16], b_height))

        self.deep_orphan_B(parent, b_hash, b_height)
        self.log.info("[A] parent reorged deep: B orphaned; new best height %d"
                      % parent.getblockcount())

        # The whole anchored chain must roll back to genesis.
        self.wait_until_count(seq, 0, timeout=120)
        assert_equal(seq.getbestblockhash(), seq.getblockhash(0))
        assert_equal(seq.getblockheader(orphaned_b1_a)['confirmations'], -1)
        self.log.info("[A] Sequentia rolled all the way back to genesis")

        # Rebuild on the parent's new best chain.
        for _ in range(3):
            seq.generateposblock(self.staker_wif)
        self.wait_until_count(seq, 3, timeout=60)
        assert seq.getblockhash(1) != orphaned_b1_a
        new_hdr = seq.getblockheader(seq.getbestblockhash())
        assert (new_hdr['anchorheight'], new_hdr['anchorhash']) != (b_height, b_hash)
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')

        # A fresh node syncs the canonical (rebuilt) chain, never the orphan.
        canonical_tip_a = seq.getbestblockhash()
        self.start_node(2)
        self.connect_nodes(1, 2)
        self.wait_until_count(fresh, 3, timeout=120)
        assert_equal(fresh.getbestblockhash(), canonical_tip_a)
        for n in (seq, fresh):
            assert_equal(n.getbestblockhash(), canonical_tip_a)
            assert n.getbestblockhash() != orphaned_tip_a
        self.log.info("[A] fresh node synced canonical chain; no node stuck")

        # ============================================================
        # Scenario B — a buried checkpoint floor must RETREAT when its own
        # parent-chain commitment is orphaned by the deep reorg.
        # ============================================================
        # Keep the fresh node out of the way for the controlled reorg.
        self.disconnect_nodes(1, 2)

        # Advance the parent to a fresh still tip B2, then produce more SEQ
        # blocks all anchored to B2.
        self.generatetoaddress(parent, 5, addr, sync_fun=self.no_op)
        start = seq.getblockcount()
        b2_height, b2_hash = self.produce_all_anchored_to_still_B(seq, parent, 4)
        ckpt_height = seq.getblockcount()           # checkpoint this tip
        ckpt = seq.getcheckpointpayload()
        assert_equal(ckpt['height'], ckpt_height)

        # Commit the checkpoint into the parent (in a block AFTER B2, so the
        # reorg that orphans B2 also orphans the commitment) and bury it
        # CHECKPOINT_DEPTH deep, so it finalizes ckpt_height.
        ckpt_txid = self.commit_checkpoint(parent, ckpt['payload'])
        self.generatetoaddress(parent, CHECKPOINT_DEPTH, addr, sync_fun=self.no_op)

        deadline = time.time() + 30
        while time.time() < deadline and seq.getcheckpointinfo()['finalized_height'] != ckpt_height:
            time.sleep(0.25)
        assert_equal(seq.getcheckpointinfo()['finalized_height'], ckpt_height)
        assert_equal(seq.getcheckpointinfo()['finalized_hash'], ckpt['blockhash'])
        self.log.info("[B] checkpoint finalized SEQ height %d (commitment buried in parent)"
                      % ckpt_height)

        # Extend a little past the checkpoint.
        for _ in range(2):
            seq.generateposblock(self.staker_wif)
        orphaned_tip_b = seq.getbestblockhash()
        orphaned_ckpt_block = seq.getblockhash(ckpt_height)
        assert_equal(orphaned_ckpt_block, ckpt['blockhash'])
        first_b2_block = seq.getblockhash(start + 1)

        # DEEP reorg orphaning B2: this kills the anchor of EVERY block from
        # start+1 up (all anchored to B2 or its now-orphaned descendants) AND
        # the checkpoint's own commitment (mined at/after B2). Per spec the
        # finalized point must retreat and the chain roll back below the
        # checkpoint — finality is modulo a Bitcoin reorg.
        self.deep_orphan_B(parent, b2_hash, b2_height, evict_txids=[ckpt_txid])
        # The checkpoint's commitment is genuinely gone from the parent's best
        # chain (double-spent on the new branch), not merely re-mined.
        assert parent.gettransaction(ckpt_txid)['confirmations'] <= 0
        self.log.info("[B] parent reorged deep: B2 + checkpoint commitment orphaned; new best %d"
                      % parent.getblockcount())

        # The chain must roll back to the last anchor-canonical block, which is
        # the genesis-relative pre-B2 tip (height `start`). The checkpointed
        # block — whose anchor and whose own commitment are now off Bitcoin's
        # best chain — must NOT be kept.
        self.wait_until_count(seq, start, timeout=120)
        assert_equal(seq.getblockheader(first_b2_block)['confirmations'], -1)
        assert_equal(seq.getblockheader(orphaned_ckpt_block)['confirmations'], -1)
        assert seq.getbestblockhash() != orphaned_tip_b
        # The finalized point retreated below the rolled-back tip.
        assert seq.getcheckpointinfo()['finalized_height'] <= start
        self.log.info("[B] chain rolled back below the orphaned checkpoint; finality retreated")

        # Production resumes on the parent's new best chain, past the old
        # checkpoint height, on a fresh anchor.
        target = ckpt_height + 1
        for _ in range(target - start):
            seq.generateposblock(self.staker_wif)
        self.wait_until_count(seq, target, timeout=60)
        assert seq.getblockhash(ckpt_height) != orphaned_ckpt_block, \
            "checkpointed height must be rebuilt with a NEW, anchor-canonical block"
        assert_equal(seq.getanchorstatus()['anchorstatus'], 'ok')

        # The fresh node syncs the rebuilt canonical chain and is not stuck.
        canonical_tip_b = seq.getbestblockhash()
        self.connect_nodes(1, 2)
        self.wait_until_count(fresh, target, timeout=120)
        assert_equal(fresh.getbestblockhash(), canonical_tip_b)
        for n in (seq, fresh):
            assert n.getbestblockhash() != orphaned_tip_b
        self.log.info("[B] fresh node synced the rebuilt canonical chain; no node stuck")


if __name__ == '__main__':
    PosDeepAnchorReorgTest().main()
