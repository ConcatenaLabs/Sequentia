#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The replay of lost blocks (doc 03, "Transactions of a lost block") measured
at the network's scale, one factor at a time, with the autonomous committee
at the testnet's round lengths. Not part of the default test list: a run takes
from ten minutes to several hours, and needs Bitcoin Core (PARENT_BITCOIND).

Every factor is an environment variable, and SCALE_OUT names the JSON file a
run writes:

  SCALE_DEPTH      Bitcoin blocks replaced (default 3)
  SCALE_PER_BTC    Sequentia blocks on each of them (default 10)
  SCALE_TXS        transactions per lost block (default 480)
  SCALE_COLD=1     restart every node between the reorganisation and the replay
  SCALE_CT=1       confidential transactions, with range and surjection proofs
  SCALE_MEMBERS    committee members (default 6). Above 6 the members are real
                   registered stakers shared among the six nodes, so that every
                   certificate is a real BLS aggregate of that many keys
  SCALE_THROTTLE   every node runs at 1/x of this machine's speed: SIGSTOP and
                   SIGCONT on a 50 ms cycle, so that every thread slows alike
  SCALE_FAIL       claims that fail in the replay (their payment proof names a
                   Bitcoin block that was replaced)
  SCALE_BTC_EVERY  a Bitcoin block every so many seconds during the replay, so
                   that proposals at one height carry different anchors
  SCALE_TRAFFIC    ordinary transactions per second sent during the replay
                   (default 2), which is what the backlog is counted from
  SCALE_SPACING    the minimum block spacing, in seconds, as a consensus rule
                   (the testnet's is 60). The chain is built on a mock clock in
                   the past and the clock returns to real time before anything
                   is measured, so that only the measured part runs at it
  SCALE_EXTRA      further node arguments, comma-separated
  SCALE_RULE=off   the lost-block rule off on every node: disconnected
                   transactions return to the mempool and compete by fee
  SCALE_PROBES=1   just after the reorganisation, three new transactions at
                   each of four fee levels, each timed until it is in a block
  SCALE_SOAK       seconds of repeated reorganisations instead of one replay:
                   a 1-block Bitcoin reorganisation every 2 or 3 Bitcoin blocks
                   (one Bitcoin block per SCALE_SOAK_BTC seconds, default 100),
                   with blocks kept full by SCALE_SOAK_TPS transactions a second
"""

import json
import os
import signal
import statistics
import threading
import time
from decimal import Decimal

import feature_btc_payment_proof as bp
from feature_btc_payment_proof import BtcPaymentProofTest
from feature_pos_lost_autonomous import LostAutonomousTest
from feature_pos_lost_blocks import ALL_KEYS, HONEST, N, SELLER, make_key
from feature_pos_lost_swap import LostSwapTest
from test_framework.address import program_to_witness
from test_framework.authproxy import JSONRPCException
from test_framework.key import compute_xonly_pubkey, generate_privkey
from test_framework.messages import tx_from_hex
from test_framework.script import CScript
from test_framework.test_node import get_rpc_proxy

DEPTH = int(os.environ.get("SCALE_DEPTH", "3"))
PER_BTC = int(os.environ.get("SCALE_PER_BTC", "10"))
TXS = int(os.environ.get("SCALE_TXS", "480"))
COLD = os.environ.get("SCALE_COLD", "0") == "1"
CT = os.environ.get("SCALE_CT", "0") == "1"
MEMBERS = int(os.environ.get("SCALE_MEMBERS", str(N)))
THROTTLE = float(os.environ.get("SCALE_THROTTLE", "1"))
FAIL = int(os.environ.get("SCALE_FAIL", "0"))
BTC_EVERY = float(os.environ.get("SCALE_BTC_EVERY", "0"))
TRAFFIC = float(os.environ.get("SCALE_TRAFFIC", "2"))
SOAK = int(os.environ.get("SCALE_SOAK", "0"))
SOAK_BTC = float(os.environ.get("SCALE_SOAK_BTC", "100"))
SOAK_TPS = float(os.environ.get("SCALE_SOAK_TPS", "40"))
SPACING = int(os.environ.get("SCALE_SPACING", "0"))
SPACING_BACK = 18000  # how far in the past the mock clock starts
MOCK_START = int(time.time()) - SPACING_BACK
RULE_ON = os.environ.get("SCALE_RULE", "on") != "off"
PROBES = os.environ.get("SCALE_PROBES", "0") == "1"
# Against the 0.0001 every transaction of the lost branch pays.
PROBE_FEES = [("below", Decimal("0.00002")), ("same", Decimal("0.0001")), ("10x", Decimal("0.001")), ("100x", Decimal("0.005"))]
PROBES_PER_FEE = 3
OUT = os.environ.get("SCALE_OUT", "")
TRAFFIC_POOL = 900


class LostScaleTest(LostAutonomousTest):
    proof_for = BtcPaymentProofTest.proof_for
    link_headers = BtcPaymentProofTest.link_headers
    spend_lock = BtcPaymentProofTest.spend_lock
    claim = BtcPaymentProofTest.claim
    btc_rpc = LostSwapTest.btc_rpc
    recorded_anchor_at_or_above = LostSwapTest.recorded_anchor_at_or_above
    wallets = LostSwapTest.wallets
    coin_anchor = LostSwapTest.coin_anchor
    anchored_block = LostSwapTest.anchored_block

    def set_test_params(self):
        super().set_test_params()
        self.rpc_timeout = 600
        # Every member's keys: the node index is a group of real stakers.
        self.stakers = [make_key() for _ in range(max(MEMBERS, N))]
        self.groups = [list(range(i, max(MEMBERS, N), N)) for i in range(N)]
        # As on the testnet, the wallet that funds the load is on a node that
        # neither produces nor signs (node 0): the others hold every staker key
        # and are the ones timed. A wallet's own upkeep is not part of a replay.
        self.producers = {i: [] for i in range(1, N)}
        for k in range(max(MEMBERS, N)):
            self.producers[1 + k % (N - 1)].append(k)
        for i in range(N):
            self.extra[i] = [a for a in self.extra[i] if not a.startswith("-debug=net")]
            self.extra[i] += ["-dbcache=64", "-maxmempool=200", "-debug=validation"]

    def node_args(self, i):
        # Every node honest, at the testnet's Bitcoin poll, checking scripts on
        # four threads rather than the one the attack suites use for their errors.
        swap = {"-anchorpollinterval=1": "-anchorpollinterval=15", "-par=1": "-par=%s" % os.environ.get("SCALE_PAR", "4")}
        # The escaping stall keeps the real-time gap the testnet requires, so
        # that a sub-quorum block cannot stand in for the first certified height.
        drop = {"-posdebugignorelost=1", "-posescapestallmtpgap=0"}
        args = [swap.get(a, a) for a in super().node_args(i) if a not in drop]
        # A regtest node re-checks its whole mempool at every change, under the
        # chain lock. The testnet does not, and with thousands of lost
        # transactions pooled that check would be most of what is measured.
        args += ["-checkmempool=0", "-checkblockindex=0"]
        args += [a for a in os.environ.get("SCALE_EXTRA", "").split(",") if a]
        if not RULE_ON:
            args.append("-posdebugignorelost=1")
        if SPACING:
            args += ["-posblockspacing=%d" % SPACING, "-posblockspacingheight=1", "-mocktime=%d" % getattr(self, "mock", MOCK_START)]
        if MEMBERS > N:
            args = [("-poscommitteesize=%d" % MEMBERS) if a.startswith("-poscommitteesize=") else a for a in args]
        return args

    # ---------------------------------------------------------- the committee
    def advance_mock(self, node_i):
        """The mock clock moves to the earliest time the next block may carry."""
        self.mock = max(getattr(self, "mock", MOCK_START), self.tip(self.nodes[node_i])["time"] + SPACING)
        for n in self.nodes:
            if n.running:
                n.setmocktime(self.mock)

    def end_mock(self):
        if SPACING:
            assert self.mock < time.time(), "the mock clock ran past real time"
            for n in self.nodes:
                n.setmocktime(0)
            self.mock_over = True

    def produce(self, node_i, signers, leaders=None, txs_must=(), allow_fail=False):
        if SPACING and not getattr(self, "mock_over", False):
            self.advance_mock(node_i)
        if MEMBERS <= N:
            return super().produce(node_i, signers, leaders, txs_must, allow_fail)
        node = self.nodes[node_i]
        leaders = leaders if leaders is not None else signers
        if txs_must:
            self.wait_until(lambda: all(t in node.getrawmempool() for t in txs_must), timeout=60)
        keys = [k for g in signers for k in self.groups[g]]
        last = None
        for _ in range(80):
            for g in leaders:
                for k in self.groups[g][:6]:
                    try:
                        r = node.generateposblock(self.stakers[k][0], [self.stakers[j][0] for j in keys if j != k])
                        b = node.getblock(r["hash"])
                        for t in txs_must:
                            assert t in b["tx"], "block lacks %s" % t
                        r["block"] = b
                        return r
                    except JSONRPCException as e:
                        last = e.error["message"]
            time.sleep(0.25)
        if allow_fail:
            return {"error": last}
        raise AssertionError("no block on node %d: %s" % (node_i, last))

    def start_producers(self, idx):
        for i in idx:
            r = self.nodes[i].startposproducer([self.stakers[k][0] for k in self.producers[i]])
            assert r["producing"], r

    # ---------------------------------------------------------------- coins
    def fans(self, count, each, amount, confidential):
        """`count` mined transactions of `each` outputs; returns [(txid, vout)]."""
        a = self.nodes[SELLER]
        coins = [self.fund(amount * each + Decimal("1")) for _ in range(count)]
        made = []
        for coin in coins:
            addrs = [a.getnewaddress("", "blech32" if confidential else "bech32") for _ in range(each)]
            outs = [{ad: amount} for ad in addrs]
            outs.append({a.getnewaddress(): coin["amount"] - amount * each - Decimal("0.001")})
            outs.append({"fee": Decimal("0.001")})
            raw = a.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}], outs)
            if confidential:
                raw = a.blindrawtransaction(raw)
            txid = a.sendrawtransaction(a.signrawtransactionwithwallet(raw)["hex"])
            plain = {a.getaddressinfo(ad).get("unconfidential", ad) for ad in addrs}
            made.append((txid, plain))
        while any(t in a.getrawmempool() for t, _ in made):
            self.produce(SELLER, ALL_KEYS)
        self.sync_tips(range(N), timeout=300)
        out = []
        for txid, plain in made:
            for o in a.getrawtransaction(txid, 1)["vout"]:
                if o["scriptPubKey"].get("address") in plain:
                    out.append((txid, o["n"]))
        # Locked, so that a later wallet send cannot spend a coin a transaction
        # signed here already spends.
        a.lockunspent(False, [{"txid": t, "vout": n} for t, n in out])
        return out

    def leaf(self, coin, amount, dest, confidential, fee=Decimal("0.0001")):
        a = self.nodes[SELLER]
        raw = a.createrawtransaction([{"txid": coin[0], "vout": coin[1]}], [{dest: amount - fee}, {"fee": fee}])
        if confidential:
            raw = a.blindrawtransaction(raw)
        return a.signrawtransactionwithwallet(raw)["hex"]

    def failing_claims(self, n):
        """n claims, each valid against a payment in the next Bitcoin block.
        Replacing that block makes every proof name a block off the chain, so
        each claim fails in the replay."""
        self.wallets()
        b = self.btc()
        a = self.nodes[SELLER]
        # One small payment per few claims, as real ones are: a claim carries
        # its payment in its proof. Each is funded by its own confirmed coin.
        per_payment = 4
        payments = (n + per_payment - 1) // per_payment
        self.bbuyer.sendmany("", {self.bbuyer.getnewaddress("", "bech32"): Decimal("0.01") for _ in range(payments)})
        b.generatetoaddress(1, self.bseller.getnewaddress())
        h0 = b.getblockcount()
        sat = 10000
        sk_b, sk_s = generate_privkey(), generate_privkey()
        pk_b, pk_s = compute_xonly_pubkey(sk_b)[0], compute_xonly_pubkey(sk_s)[0]
        floor, deadline, refund_at = h0, h0 + 500, h0 + 400
        locks = []
        outs = []
        for _ in range(n):
            addr = self.bseller.getnewaddress("", "bech32")
            info = self.bseller.getaddressinfo(addr)
            program, version = bytes.fromhex(info["witness_program"]), info["witness_version"]
            script = CScript([pk_b, bp.OP_CHECKSIGVERIFY, bp.OP_INSPECTBTCPAYMENT, bp.OP_DROP, bp.OP_DUP, floor, bp.OP_GREATERTHAN,
                              bp.OP_VERIFY, deadline, bp.OP_LESSTHANOREQUAL, bp.OP_VERIFY, bp.FIELD_OUTPUT, bp.OP_INSPECTBTCTX, version,
                              bp.OP_EQUALVERIFY, program, bp.OP_EQUALVERIFY, bp.le8(sat), bp.OP_GREATERTHANOREQUAL64])
            refund = CScript([refund_at, bp.OP_CHECKANCHORHEIGHTVERIFY, bp.OP_DROP, pk_s, bp.OP_CHECKSIG])
            tap = bp.taproot_construct(bp.NUMS, [("claim", script), ("refund", refund)])
            lock_addr = program_to_witness(1, tap.scriptPubKey[2:])
            locks.append(dict(sk_b=sk_b, sk_s=sk_s, tap=tap, claim=script, refund=refund, addr=addr, amount=sat, floor=floor,
                              deadline=deadline, refund_at=refund_at, asset_amount=10000000))
            outs.append({lock_addr: Decimal("0.1")})
        # At most 400 lock outputs per transaction, so that each fits a block.
        for start in range(0, n, 400):
            part = locks[start:start + 400]
            raw = a.createrawtransaction([], outs[start:start + 400])
            funded = a.fundrawtransaction(raw, {"fee_asset": "bitcoin", "change_type": "bech32"})["hex"]
            t_hex = a.signrawtransactionwithwallet(funded)["hex"]
            t = tx_from_hex(t_hex)
            txid = t.rehash()
            for lock in part:
                vout = [i for i, o in enumerate(t.vout) if o.scriptPubKey == lock["tap"].scriptPubKey][0]
                lock.update(txid=txid, vout=vout, txout=t.vout[vout])
            self.nodes[1].sendrawtransaction(t_hex)
            self.produce(1, ALL_KEYS, txs_must=[txid])
        for _ in range(bp.FLOOR_DEPTH + 1):
            self.produce(1, ALL_KEYS)
        self.sync_tips(range(N), timeout=300)
        pays = []
        for start in range(0, n, per_payment):
            part = locks[start:start + per_payment]
            txid = self.bbuyer.sendmany("", {lock["addr"]: Decimal(sat) / Decimal(100000000) for lock in part}, 1)
            pays.append(txid)
            for lock in part:
                lock["pay"] = txid
        return locks, pays

    def claims_for(self, locks, blockhash):
        funder = self.nodes[SELLER]
        spk = bytes.fromhex(funder.getaddressinfo(funder.getnewaddress())["scriptPubKey"])
        vouts = {}
        proofs = {}
        anchors = {}
        out = []
        for lock in locks:
            pay = lock["pay"]
            if pay not in vouts:
                vouts[pay] = {o["scriptPubKey"].get("address"): o["n"] for o in self.btc().getrawtransaction(pay, True, blockhash)["vout"]}
            if lock["txid"] not in anchors:
                anchors[lock["txid"]] = self.coin_anchor(lock["txid"])
            anchor = anchors[lock["txid"]]
            if (pay, anchor) not in proofs:
                proofs[(pay, anchor)] = self.proof_for(pay, anchor, blockhash)["record"]
            out.append(self.claim(lock, proofs[(pay, anchor)], vouts[pay][lock["addr"]], out_spk=spk).serialize().hex())
        return out

    # --------------------------------------------------------------- building
    def build_branch(self, case):
        a = self.nodes[SELLER]
        n1 = self.nodes[1]
        nblocks = DEPTH * PER_BTC
        amount = Decimal("0.01")
        each = min(TXS, 40) if CT else TXS
        count = (nblocks * TXS + each - 1) // each
        t0 = time.time()
        fails = []
        fail_ids = set()
        claim_weight = 0
        if FAIL:
            locks, pays = self.failing_claims(FAIL)
        coins = self.fans(count, each, amount, CT)
        spare = self.fans(max(1, TRAFFIC_POOL // 400), 400, amount, False)
        self.record(case, "coins", "%d + %d coins in %.0f s" % (len(coins), len(spare), time.time() - t0))
        dest = a.getnewaddress("", "blech32" if CT else "bech32")
        hexes = [self.leaf(c, amount, dest, CT) for c in coins[:nblocks * TXS]]
        plain_dest = a.getnewaddress()
        self.probes = []
        if PROBES:
            want = len(PROBE_FEES) * PROBES_PER_FEE
            probe_coins, spare = spare[-want:], spare[:-want]
            for k, c in enumerate(probe_coins):
                level, fee = PROBE_FEES[k // PROBES_PER_FEE]
                self.probes.append((level, self.leaf(c, amount, plain_dest, False, fee)))
        traffic = [self.leaf(c, amount, plain_dest, False) for c in spare[:TRAFFIC_POOL]]
        self.record(case, "signed", "%d transactions and %d of traffic in %.0f s" % (len(hexes), len(traffic), time.time() - t0))
        if SPACING:
            # The lost branch ends about now, as a real one does: the fork
            # point is as old as the branch and no older.
            self.mock = max(self.mock, int(time.time()) - SPACING * (nblocks + 2))
        f_height = n1.getblockcount()
        blocks = []
        for d in range(DEPTH):
            mined = self.btc_mine(1)
            if FAIL and d == 0:
                # The blocks that anchor the payment are part of the lost branch.
                self.anchored_block(self.btc().getblockcount())
                in_block = set(self.btc().getblock(mined[0])["tx"])
                assert all(t in in_block for t in pays), "a payment is not in the block"
                fails = self.claims_for(locks, mined[0])
                claim_weight = n1.decoderawtransaction(fails[0])["weight"]
                fail_ids = {tx_from_hex(hx).rehash() for hx in fails}
            for k in range(PER_BTC):
                index = d * PER_BTC + k
                batch = hexes[index * TXS:(index + 1) * TXS]
                share = []
                if fails:
                    per = (FAIL + nblocks - 1) // nblocks
                    share, fails = fails[:per], fails[per:]
                    # The block stays as full as the others: a claim displaces
                    # plain payments of about 770 weight units each.
                    batch = batch[:max(0, len(batch) - (claim_weight * len(share)) // 770 - 1)]
                ids = [n1.sendrawtransaction(hx) for hx in share + batch]
                must = ids[:len(share)] if share else ids[:1]
                blocks.append(self.produce(1, ALL_KEYS, leaders=[1, 2, 3, 4], txs_must=must))
        self.sync_tips(range(N), timeout=600)
        lost = []
        weights = []
        for L in blocks:
            blk = n1.getblock(L["hash"])
            lost += blk["tx"][1:]
            weights.append(blk["weight"])
        self.record(case, "branch", "%d blocks, heights %d..%d, %d transactions, weights %d..%d" %
                    (len(blocks), blocks[0]["height"], blocks[-1]["height"], len(lost), min(weights), max(weights)))
        assert fail_ids <= set(lost), "a claim is not in the lost branch"
        self.end_mock()
        return blocks, lost, traffic, f_height, fail_ids

    # ------------------------------------------------------------ what is timed
    def restart_all(self):
        for i in range(N):
            self.stop_node(i)
        for i in range(N):
            self.start_node(i, extra_args=self.node_args(i))
        self.mesh(range(N))

    def throttler(self, stop):
        cycle = 0.05
        run = cycle / THROTTLE
        pids = [self.nodes[i].process.pid for i in range(N)]
        try:
            while not stop.is_set():
                time.sleep(run)
                for p in pids:
                    os.kill(p, signal.SIGSTOP)
                time.sleep(cycle - run)
                for p in pids:
                    os.kill(p, signal.SIGCONT)
        finally:
            for p in pids:
                try:
                    os.kill(p, signal.SIGCONT)
                except ProcessLookupError:
                    pass

    def sender(self, hexes, stop, sent):
        rpc = get_rpc_proxy(self.nodes[2].url, 2, timeout=120)
        gap = 1.0 / TRAFFIC if TRAFFIC > 0 else 0
        for hx in hexes:
            if stop.is_set() or not TRAFFIC:
                return
            try:
                sent.append(rpc.sendrawtransaction(hx))
            except Exception:
                pass
            stop.wait(gap)

    def btc_miner(self, stop, mined):
        while not stop.wait(BTC_EVERY):
            try:
                self.btc().generatetoaddress(1, self.btc_addr)
                mined.append(time.time())
            except Exception:
                pass

    def plan_rows(self, since):
        """Per height: every plan the nodes computed, in microseconds."""
        rows = {}
        for i in range(1, N):
            for line in self.log_has(i, "plan computed on", since[i]):
                h = int(line.split(" at height ")[1].split(":")[0])
                us = int(line.split("classified in ")[1].split(" us")[0])
                rows.setdefault(h, []).append((i, us))
        return rows

    def wallet_sweeps(self, since):
        """What the wallet's rejection sweep did, from the measurement binary's counters."""
        out = {}
        for label, nodes in (("timed_nodes", range(1, N)), ("funding_node", [0])):
            found = []
            for i in nodes:
                for line in self.log_has(i, "MEASURE wallet rejection sweep", since[i]):
                    found.append({k: int(v) for k, v in (x.split("=") for x in line.split("sweep: ")[1].split())})
            out[label] = dict(sweeps=len(found), max_candidates=max((f["candidates"] for f in found), default=0),
                              held_ms_total=round(sum(f["cs_wallet_held_us"] for f in found) / 1000, 1),
                              held_ms_max=round(max((f["cs_wallet_held_us"] for f in found), default=0) / 1000, 1))
        return out

    def round_stats(self, chain, since):
        out = {}
        for h, bh in chain:
            rnd, members = self.round_of(bh, since)
            out[h] = (rnd, members)
        return out

    def recorded_inputs(self, since):
        """Inputs bound by the records node 1 has connected, and the blocks carrying them."""
        lines = self.log_has(1, " bound input(s)", since)
        lines = [l for l in lines if " records " in l]
        return sum(int(l.split(" records ")[1].split(" ")[0]) for l in lines), len(lines)

    def replay(self, case, blocks, lost, traffic, f_height, fail_ids=frozenset()):
        since = [self.log_size(i) for i in range(N)]
        t_reorg = time.time()
        self.bitcoin_reorg(DEPTH)
        self.wait_until(lambda: all(self.nodes[i].getblockcount() <= f_height for i in range(1, N)), timeout=600)
        self.wait_lost(blocks[-1]["hash"], list(range(1, N)), timeout=600)
        self.record(case, "held", "every node holds the loss %.1f s after the reorganisation" % (time.time() - t_reorg))
        if COLD:
            t = time.time()
            self.restart_all()
            self.wait_lost(blocks[-1]["hash"], list(range(1, N)), timeout=600)
            self.record(case, "restart", "every node restarted in %.1f s" % (time.time() - t))
            since = [self.log_size(i) for i in range(N)]
        probe_sent = {}
        probe_wait = {}
        for level, hx in self.probes:
            txid = self.nodes[2].sendrawtransaction(hx)
            probe_sent[txid] = (level, time.time())
        stop = threading.Event()
        threads = []
        sent = []
        mined = []
        threads.append(threading.Thread(target=self.sender, args=(traffic, stop, sent), daemon=True))
        if BTC_EVERY:
            threads.append(threading.Thread(target=self.btc_miner, args=(stop, mined), daemon=True))
        if THROTTLE > 1:
            threads.append(threading.Thread(target=self.throttler, args=(stop,), daemon=True))
        # A failing claim is never carried: it is done when a record binds it.
        left = set(lost) - set(fail_ids)
        every_lost = set(lost)
        n1 = self.nodes[1]
        first_ordinary = None
        t0 = time.time()
        start_after_reorg = t0 - t_reorg
        for th in threads:
            th.start()
        self.start_producers(range(1, N))
        first = None
        chain = []
        carried_at = {}
        h = f_height
        stall_limit = float(os.environ.get("SCALE_STALL_LIMIT", "1800"))
        timed_out = False
        try:
            # Until the replay is done and a block has again taken a new
            # transaction: what the replay held back is the wait for that block.
            while left or self.recorded_inputs(since[1])[0] < len(fail_ids) or (TRAFFIC and first_ordinary is None) or len(probe_wait) < len(probe_sent):
                try:
                    self.wait_until(lambda: n1.getblockcount() >= h + 1, timeout=stall_limit)
                except AssertionError:
                    timed_out = True
                    break
                h += 1
                if first is None:
                    first = time.time() - t0
                bh = n1.getblockhash(h)
                txs = n1.getblock(bh)["tx"]
                carried = [t for t in txs[1:] if t in left]
                left -= set(carried)
                chain.append((h, bh))
                for t in txs[1:]:
                    if t in probe_sent and t not in probe_wait:
                        probe_wait[t] = (probe_sent[t][0], round(time.time() - probe_sent[t][1], 1))
                ordinary = [t for t in txs[1:] if t not in every_lost and t not in probe_sent]
                if first_ordinary is None and set(ordinary) & set(sent):
                    first_ordinary = time.time() - t0
                carried_at[h] = (len(carried), round(time.time() - t0, 1), len(ordinary),
                                 round(time.time() - n1.getblockheader(bh)["time"], 1))
                if h > f_height + len(blocks) + 60:
                    break
            end = time.time() - t0
            pool = set(n1.getrawmempool())
            sent_now = set(sent)
        finally:
            stop.set()
            for th in threads:
                th.join(timeout=30)
        rows = self.plan_rows(since)
        rounds = self.round_stats(chain, since)
        restarts = sum(len(self.log_has(i, "restarting collection", since[i])) for i in range(1, N))
        table = []
        certified = {h_: n1.getblockheader(bh_)["poscertified"] for h_, bh_ in chain}
        for bh_h, _ in chain:
            plans = rows.get(bh_h, [])
            us = [u for _, u in plans]
            table.append(dict(height=bh_h, certified=certified[bh_h], carried=carried_at[bh_h][0], new_txs=carried_at[bh_h][2], stamp_behind_clock_s=carried_at[bh_h][3], at_s=carried_at[bh_h][1], round=rounds[bh_h][0],
                              members=rounds[bh_h][1], plans=len(us), plan_ms_max=round(max(us) / 1000, 1) if us else None,
                              plan_ms_median=round(statistics.median(us) / 1000, 1) if us else None))
        failed = sum(r["round"] for r in table if r["round"])
        all_us = [u for v in rows.values() for _, u in v]
        bound, record_blocks = self.recorded_inputs(since[1])
        first_cert = next((r["at_s"] for r in table if r["certified"]), None)
        last_carry = max((r["at_s"] for r in table if r["carried"]), default=None)
        behind = [(r["at_s"], r["stamp_behind_clock_s"]) for r in table]
        back_at_clock = next((a for a, b in behind if b <= 90), None)
        waits = {level: sorted(w for lv, w in probe_wait.values() if lv == level) for level, _ in PROBE_FEES} if probe_sent else {}
        summary = dict(wallet_sweeps=self.wallet_sweeps(since), rule_on=RULE_ON, lost_blocks=len(blocks), heights_carrying_lost=sum(1 for r in table if r["carried"]),
                       new_tx_wait_s=waits, new_tx_not_mined=len(probe_sent) - len(probe_wait),
                       producers_started_after_reorg_s=round(start_after_reorg, 1), replay_done_s=last_carry,
                       stamp_behind_clock_first_s=behind[0][1] if behind else None,
                       stamp_behind_clock_last_s=behind[-1][1] if behind else None, chain_back_at_clock_s=back_at_clock,
                       no_new_tx_for_s=round(first_ordinary, 1) if first_ordinary is not None else None,
                       first_quorum_certified_s=first_cert, heights_below_quorum=sum(1 for r in table if not r["certified"]),
                       failing=len(fail_ids), inputs_recorded=bound, blocks_with_records=record_blocks, heights=len(table), replay_s=round(end, 1), first_block_s=round(first, 1) if first else None,
                       rounds_failed=failed, collections_restarted=restarts, left_uncarried=len(left), timed_out=timed_out,
                       backlog_at_end=len(pool & sent_now), traffic_sent=len(sent_now), bitcoin_blocks_during=len(mined),
                       plans_computed=len(all_us),
                       plan_ms_max=round(max(all_us) / 1000, 1) if all_us else None,
                       plan_ms_median=round(statistics.median(all_us) / 1000, 1) if all_us else None,
                       plan_s_total_per_node=round(sum(all_us) / 1e6 / N, 2))
        self.record(case, "summary", summary)
        self.results.setdefault(case, {})["per_height"] = table
        return summary

    # -------------------------------------------------------------------- soak
    def soak_coins(self, count):
        return self.fans(max(1, count // 400), 400, Decimal("0.01"), False)[:count]

    def soak_sender(self, coins, stop, stats):
        a = self.nodes[SELLER]
        rpc = get_rpc_proxy(self.nodes[2].url, 2, timeout=120)
        dest = a.getnewaddress()
        k = 0
        while not stop.is_set():
            t = time.time()
            n = int(SOAK_TPS)
            for _ in range(n):
                slot = k % len(coins)
                coin = coins[slot]
                k += 1
                # A coin is spent again and again, one fee smaller each time.
                value = coin[2] if len(coin) > 2 else Decimal("0.01")
                if value < Decimal("0.001"):
                    continue
                try:
                    hx = self.leaf(coin, value, dest, False)
                    txid = rpc.sendrawtransaction(hx)
                    coins[slot] = (txid, 0, value - Decimal("0.0001"))
                    stats["sent"] += 1
                except Exception:
                    stats["refused"] += 1
                if stop.is_set():
                    return
            stop.wait(max(0.0, 1.0 - (time.time() - t)))

    def soak(self, case):
        t_build = time.time()
        coins = self.soak_coins(int(SOAK_TPS * 600))
        self.record(case, "coins", "%d coins in %.0f s" % (len(coins), time.time() - t_build))
        self.end_mock()
        since = [self.log_size(i) for i in range(N)]
        self.start_producers(range(1, N))
        stop = threading.Event()
        stats = dict(sent=0, refused=0)
        th = threading.Thread(target=self.soak_sender, args=(coins, stop, stats), daemon=True)
        th.start()
        n1 = self.nodes[1]
        t0 = time.time()
        h0 = n1.getblockcount()
        reorgs = []
        samples = []
        next_btc = t0 + SOAK_BTC
        until_reorg = 2
        try:
            while time.time() - t0 < SOAK:
                time.sleep(max(0.0, min(5.0, next_btc - time.time())))
                samples.append((round(time.time() - t0), n1.getblockcount(), n1.getmempoolinfo()["size"]))
                if time.time() < next_btc:
                    continue
                next_btc += SOAK_BTC
                if until_reorg > 0:
                    self.btc_mine(1)
                    until_reorg -= 1
                    continue
                tip = n1.getblockcount()
                self.bitcoin_reorg(1)
                reorgs.append(dict(at_s=round(time.time() - t0), tip=tip))
                until_reorg = 2 + (len(reorgs) % 2)
        finally:
            stop.set()
            th.join(timeout=60)
        rows = self.plan_rows(since)
        us = [u for v in rows.values() for _, u in v]
        heights = n1.getblockcount() - h0
        rounds_failed = 0
        for h in range(h0 + 1, n1.getblockcount() + 1):
            rnd, _ = self.round_of(n1.getblockhash(h), since)
            rounds_failed += rnd or 0
        restarts = sum(len(self.log_has(i, "restarting collection", since[i])) for i in range(1, N))
        gaps = [b[0] - a[0] for a, b in zip(samples, samples[1:]) if b[1] == a[1]]
        summary = dict(wallet_sweeps=self.wallet_sweeps(since), seconds=round(time.time() - t0), heights=heights, reorgs=len(reorgs), rounds_failed=rounds_failed,
                       collections_restarted=restarts, plans_computed=len(us),
                       plan_ms_max=round(max(us) / 1000, 1) if us else None,
                       plan_ms_median=round(statistics.median(us) / 1000, 1) if us else None,
                       plan_s_total_per_node=round(sum(us) / 1e6 / N, 1),
                       longest_flat_sample_s=max(gaps) if gaps else 0, mempool_end=n1.getmempoolinfo()["size"],
                       mempool_max=max(s[2] for s in samples), sent=stats["sent"], refused=stats["refused"])
        self.record(case, "summary", summary)
        self.results.setdefault(case, {})["samples"] = samples[::12]
        self.results[case]["reorgs"] = reorgs

    def run_test(self):
        case = os.environ.get("SCALE_NAME", "run")
        settings = dict(depth=DEPTH, per_btc=PER_BTC, txs=TXS, cold=COLD, confidential=CT, members=MEMBERS, throttle=THROTTLE,
                        failing=FAIL, btc_every=BTC_EVERY, traffic=TRAFFIC, soak=SOAK, spacing=SPACING, rule_on=RULE_ON, probes=PROBES,
                        extra=os.environ.get("SCALE_EXTRA", ""))
        self.record(case, "settings", settings)
        try:
            if SOAK:
                self.soak(case)
            else:
                blocks, lost, traffic, f_height, fail_ids = self.build_branch(case)
                self.replay(case, blocks, lost, traffic, f_height, fail_ids)
        finally:
            if OUT:
                with open(OUT, "w") as f:
                    json.dump(self.results, f, indent=1)
            self.stop_bitcoind()


if __name__ == '__main__':
    LostScaleTest().main()
