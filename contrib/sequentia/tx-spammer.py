#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Hold a Sequentia chain at a chosen fee floor, so a wallet can be tested against
a full block instead of an empty one.

Every fee-market behaviour worth testing -- does my transaction get in, does RBF
lift it over the queue, does CPFP rescue a parent -- is invisible on an idle
chain, because on an idle chain the answer is always yes at the relay floor.
This tool manufactures the competition: it keeps the mempool holding a few
blocks' worth of transactions that all pay a known rate, so the next block is
full and the price of entry is a number the operator chose rather than an
accident.

WHAT IT COSTS. The fees are real and they are gone: they are paid to whichever
node produces the block, which on a shared testnet is somebody else's node. The
budget, not the clock, is what ends a run -- so `--plan` prints the arithmetic
and refuses a run that cannot afford its own duration. As a rule of thumb, on a
chain with 100 kvB blocks every 60s, one hour of full blocks costs
6,000,000 vB x <rate>. At 2 US cents per vB that is $120,000 an hour; at
0.05 cents it is $3,000.

TWO WAYS TO USE IT.

  Local regtest (free, and the honest place for heavy fill). Start a small
  private chain with run-local-testnet.py --blockmaxweight, point this at the
  producer's RPC, and shrink the block until one transaction is a whole block.
  Fees there cost nothing real.

  Shared testnet (realistic, but rationed). Run it on a node with a funded
  wallet, at a rate the budget can sustain for the length of the test, and tell
  the other operators first: a full block is a full block for everyone.

EXAMPLES

  # What would four hours at 0.05 cents/vB cost, and can this wallet afford it?
  tx-spammer.py --datadir /data/sequentia-testnet --wallet staking \\
      --rate-cents-per-vb 0.05 --duration 4h --plan

  # Split the wallet into a working set of UTXOs (once, before the first run).
  tx-spammer.py --datadir /data/sequentia-testnet --wallet staking \\
      --budget 20000 --utxos 1500 --fanout

  # Hold ~3 blocks of backlog at 0.05 cents/vB for four hours.
  tx-spammer.py --datadir /data/sequentia-testnet --wallet staking \\
      --rate-cents-per-vb 0.05 --spread 1.6 --duration 4h --budget 20000 --run

  # Put the working set back together afterwards.
  tx-spammer.py --datadir /data/sequentia-testnet --wallet staking --sweep

Ctrl-C stops a run cleanly; nothing is left locked or half-signed.
"""

import argparse
import base64
import http.client
import json
import os
import queue
import random
import signal
import socket
import sys
import threading
import time
from decimal import Decimal, ROUND_UP

# One reference unit is 10^8 reference atoms, and rates in the fee whitelist are
# "value of one whole unit of the asset, scaled by this". src/exchangerates.h.
EXCHANGE_RATE_SCALE = 100000000
COIN = 100000000
# BlockAssembler holds this much weight back for the coinbase before it takes a
# single transaction (src/node/miner.h), so it is not block space we can fill.
COINBASE_RESERVED_WEIGHT = 4000
WITNESS_SCALE_FACTOR = 4

_shutdown = threading.Event()


# --------------------------------------------------------------------------- RPC


class RPCError(Exception):
    def __init__(self, code, message):
        super().__init__("%s (code %d)" % (message, code))
        self.code = code
        self.message = message


class RPC:
    """Minimal JSON-RPC client, one HTTP connection per thread.

    Deliberately standalone: this script has to be droppable onto a node host
    that has the daemon but not the source tree, so it must not import the
    functional-test framework.
    """

    def __init__(self, host, port, user, password, wallet=None, timeout=120):
        self.host, self.port = host, port
        self.auth = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
        self.path = "/wallet/%s" % wallet if wallet else "/"
        self.timeout = timeout
        self._local = threading.local()
        self._id = 0
        self._id_lock = threading.Lock()

    def _conn(self):
        c = getattr(self._local, "conn", None)
        if c is None:
            c = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
            self._local.conn = c
        return c

    def _drop(self):
        c = getattr(self._local, "conn", None)
        if c is not None:
            try:
                c.close()
            except Exception:
                pass
            self._local.conn = None

    def call(self, method, *params, wallet_path=True, **named):
        """Positional params, or named ones -- several Elements RPCs have grown
        arguments in the middle, so counting commas is a way to get a silently
        wrong call (an asset map landing in the fee_rate slot, say)."""
        if named and params:
            raise ValueError("pass either positional or named parameters, not both")
        with self._id_lock:
            self._id += 1
            rid = self._id
        body = json.dumps({"jsonrpc": "1.0", "id": rid, "method": method,
                           "params": named if named else list(params)})
        headers = {"Authorization": "Basic " + self.auth, "Content-Type": "application/json"}
        path = self.path if wallet_path else "/"
        # One retry: a daemon that rotated its keepalive connection should not
        # look like an outage.
        for attempt in (0, 1):
            try:
                conn = self._conn()
                conn.request("POST", path, body, headers)
                resp = conn.getresponse()
                data = resp.read()
                break
            except (http.client.HTTPException, socket.error, ConnectionError):
                self._drop()
                if attempt:
                    raise
        try:
            payload = json.loads(data.decode(), parse_float=Decimal)
        except ValueError:
            raise RPCError(-1, "non-JSON reply (HTTP %d): %s" % (resp.status, data[:200]))
        if payload.get("error"):
            raise RPCError(payload["error"].get("code", -1), payload["error"].get("message", ""))
        return payload["result"]


def read_conf(datadir):
    """rpcuser/rpcpassword/rpcport from the node's own conf file.

    Flat and [chain]-sectioned keys are merged, last one wins: a node set up by
    hand keeps its credentials here rather than in a cookie, and asking the
    operator to repeat them on the command line is how they end up in shell
    history.
    """
    out = {}
    for name in ("elements.conf", "sequentia.conf", "bitcoin.conf"):
        path = os.path.join(datadir, name)
        if not os.path.exists(path):
            continue
        for line in open(path, encoding="utf8", errors="replace"):
            line = line.split("#", 1)[0].strip()
            if not line or line.startswith("[") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            out[k.strip().lower()] = v.strip()
        break
    return out


def read_auth(args):
    """RPC credentials: explicit, or from the datadir's conf, or its cookie."""
    if args.rpcuser:
        return args.rpcuser, args.rpcpassword
    if args.datadir:
        conf = read_conf(args.datadir)
        if conf.get("rpcuser") and conf.get("rpcpassword"):
            if not args.rpcport_given and conf.get("rpcport"):
                args.rpcport = int(conf["rpcport"])
            return conf["rpcuser"], conf["rpcpassword"]
    cookie = args.cookie
    if not cookie:
        if not args.datadir:
            sys.exit("need --rpcuser/--rpcpassword, or --cookie, or --datadir")
        cookie = os.path.join(args.datadir, args.chain_subdir, ".cookie")
    if not os.path.exists(cookie):
        sys.exit("no rpcuser/rpcpassword in the conf and no cookie at %s "
                 "(node not running, or wrong --chain-subdir?)" % cookie)
    user, _, pw = open(cookie, encoding="utf8").read().strip().partition(":")
    return user, pw


# ------------------------------------------------------------------- chain facts


class Chain:
    """The handful of node-side numbers the fee arithmetic depends on."""

    def __init__(self, rpc, fee_asset_arg):
        self.rpc = rpc
        info = rpc.call("getblockchaininfo", wallet_path=False)
        self.chain = info["chain"]
        self.height = info["blocks"]
        self.labels = rpc.call("dumpassetlabels", wallet_path=False)
        # The policy asset's label is whatever the chain was named with
        # -defaultpeggedassetname ("bitcoin" on testnet and mainnet, but not on a
        # custom regtest), so ask the chain rather than assuming the name.
        self.policy_asset = rpc.call("getsidechaininfo", wallet_path=False)["pegged_asset"]
        self.policy_label = next((k for k, v in self.labels.items() if v == self.policy_asset), None)
        self.rates = rpc.call("getfeeexchangerates", wallet_path=False)

        # The fee asset may be given as a label (tSEQ, USDX) or as a hex id.
        self.fee_asset_label = fee_asset_arg or self.policy_label or self.policy_asset
        if len(self.fee_asset_label) == 64 and all(c in "0123456789abcdef" for c in self.fee_asset_label.lower()):
            self.fee_asset = self.fee_asset_label.lower()
            self.fee_asset_label = next((k for k, v in self.labels.items() if v == self.fee_asset), self.fee_asset[:8])
        else:
            if self.fee_asset_label not in self.labels:
                sys.exit("unknown asset label %r; known: %s" % (self.fee_asset_label, ", ".join(sorted(self.labels))))
            self.fee_asset = self.labels[self.fee_asset_label]

        # The whitelist is keyed by label where the node has one, by id otherwise.
        rate = self.rates.get(self.fee_asset_label, self.rates.get(self.fee_asset))
        if rate is None:
            sys.exit("asset %s is not in this node's fee whitelist -- it would pay a fee "
                     "valued at zero and be rejected. Whitelist: %s"
                     % (self.fee_asset_label, ", ".join(sorted(self.rates))))
        self.fee_asset_rate = int(rate)

        mp = rpc.call("getmempoolinfo", wallet_path=False)
        self.relay_min_per_kvb = int(Decimal(str(mp["minrelaytxfee"])) * COIN)

        # getmempoolcongestion (24.7+) measures the backlog against the block the
        # node would actually build, -blockmaxweight included. Where it exists,
        # believe it; on an older node the backlog has to be inferred from bytes
        # and a block ceiling nobody reports, so that has to be supplied.
        self.congestion_rpc = True
        try:
            rpc.call("getmempoolcongestion", wallet_path=False)
        except RPCError:
            self.congestion_rpc = False

    def usable_block_vsize(self, override):
        # No RPC exposes the chain's nMaxBlockWeight, so this is the shipped
        # testnet/mainnet value (400000 weight, less the coinbase reserve). It is
        # only ever used to turn bytes into "blocks of backlog" and to project
        # cost, and --block-vsize overrides it for a chain built with a different
        # -blockmaxweight (which is the whole point of the regtest setup).
        return override or (400000 - COINBASE_RESERVED_WEIGHT) // WITNESS_SCALE_FACTOR

    def atoms_for_value(self, ref_atoms):
        """Fee-asset atoms that the node will value at `ref_atoms`.

        Mirrors ExchangeRateMap::ConvertValueToAmount, rounding up the same way,
        so a fee computed here is never one atom short of the intended rate.
        """
        return -((-ref_atoms * EXCHANGE_RATE_SCALE) // self.fee_asset_rate)

    def value_of_atoms(self, atoms):
        return (atoms * self.fee_asset_rate) // EXCHANGE_RATE_SCALE


# ----------------------------------------------------------------------- helpers


def parse_duration(s):
    units = {"s": 1, "m": 60, "h": 3600, "d": 86400}
    s = s.strip().lower()
    if s and s[-1] in units:
        return int(float(s[:-1]) * units[s[-1]])
    return int(float(s))


def amount(atoms):
    """Atoms -> the decimal string the RPC wants, without float rounding."""
    # Plain str() on a quantized zero gives "0E-8", which the RPC rejects and a
    # reader misreads.
    return "{:.8f}".format(Decimal(atoms) / COIN)


def to_atoms(value):
    return int((Decimal(str(value)) * COIN).quantize(Decimal("1"), rounding=ROUND_UP))


def fmt_usd(ref_atoms):
    """Reference atoms as a reference-unit figure, with enough digits to be read.

    A per-vB fee rate and an hourly burn differ by six orders of magnitude, and
    two decimals would print the first as $0.00.
    """
    v = Decimal(ref_atoms) / EXCHANGE_RATE_SCALE
    for places in ("0.01", "0.0001", "0.000001", "0.00000001"):
        q = v.quantize(Decimal(places))
        if q != 0 or v == 0:
            return "$%s" % q
    return "$%s" % v


def resolve_rate(args, chain):
    """The target fee rate, in reference atoms per vB.

    Three spellings of one number, because the natural unit differs by audience:
    an operator thinks in cents, a developer in atoms per kvB.
    """
    given = [x for x in (args.rate_cents_per_vb, args.rate_usd_per_vb, args.rate_atoms_per_kvb) if x is not None]
    if len(given) > 1:
        sys.exit("give only one of --rate-cents-per-vb / --rate-usd-per-vb / --rate-atoms-per-kvb")
    if args.rate_cents_per_vb is not None:
        return int(Decimal(str(args.rate_cents_per_vb)) / 100 * EXCHANGE_RATE_SCALE)
    if args.rate_usd_per_vb is not None:
        return int(Decimal(str(args.rate_usd_per_vb)) * EXCHANGE_RATE_SCALE)
    if args.rate_atoms_per_kvb is not None:
        return int(Decimal(str(args.rate_atoms_per_kvb)) / 1000)
    return None


def unconfidential(rpc, addr):
    info = rpc.call("getaddressinfo", addr)
    return info.get("unconfidential", addr)


# -------------------------------------------------------------------------- plan


def do_plan(rpc, chain, args, rate):
    block_vsize = chain.usable_block_vsize(args.block_vsize)
    per_hour_vsize = block_vsize * (3600 // args.block_seconds)
    fee_atoms_hour = chain.atoms_for_value(rate * per_hour_vsize)
    balances = rpc.call("getbalances")["mine"]["trusted"]
    have = to_atoms(balances.get(chain.fee_asset_label, balances.get(chain.fee_asset, 0)))
    budget = to_atoms(args.budget) if args.budget else have

    def hours(h):
        return "%.0f minutes" % (h * 60) if h < 1 else "%.2f hours" % h

    print("chain            %s at height %d" % (chain.chain, chain.height))
    print("fee asset        %s%s (%s)"
          % (chain.fee_asset_label,
             "  [the policy asset, tSEQ/SEQ in the GUI]" if chain.fee_asset == chain.policy_asset else "",
             chain.fee_asset[:16] + "..."))
    print("                 1 unit is valued at $%s by this node's whitelist"
          % (Decimal(chain.fee_asset_rate) / EXCHANGE_RATE_SCALE).quantize(Decimal("0.000001")))
    print("block ceiling    %d vB of transactions every %ds  (=%s vB/hour)"
          % (block_vsize, args.block_seconds, "{:,}".format(per_hour_vsize)))
    print("relay floor      %d reference atoms/kvB" % chain.relay_min_per_kvb)
    print()
    if rate is None:
        print("no --rate-* given; nothing to price.")
        return 0
    print("target rate      %s reference atoms/vB = %s/vB = %s per 250 vB transaction"
          % ("{:,}".format(rate), fmt_usd(rate), fmt_usd(rate * 250)))
    print("                 = %s %s per vB" % (amount(chain.atoms_for_value(rate)), chain.fee_asset_label))
    print("burn rate        %s %s/hour  (%s/hour)"
          % (amount(fee_atoms_hour), chain.fee_asset_label, fmt_usd(rate * per_hour_vsize)))
    print()
    print("wallet holds     %s %s" % (amount(have), chain.fee_asset_label))
    print("budget           %s %s" % (amount(budget), chain.fee_asset_label))
    affordable = budget / fee_atoms_hour if fee_atoms_hour else float("inf")
    print("affordable       %s of full blocks at this rate" % hours(affordable))
    if args.duration:
        want = parse_duration(args.duration)
        need = int(fee_atoms_hour * want / 3600)
        print("requested        %s (%s %s needed)" % (args.duration, amount(need), chain.fee_asset_label))
        if need > budget:
            sustainable = Decimal(budget) / Decimal(need) * Decimal(rate) / EXCHANGE_RATE_SCALE * 100
            print()
            print("SHORT: the budget buys %s of the %s asked for."
                  % (hours(affordable), hours(want / 3600)))
            print("       This rate would hold for the whole run instead:")
            print("         --rate-cents-per-vb %s" % sustainable.quantize(Decimal("0.0001")))
            return 1
        print()
        print("OK: the budget covers the run with %s %s to spare."
              % (amount(budget - need), chain.fee_asset_label))
    return 0


# ------------------------------------------------------------------------ fanout


def spendable(rpc, chain, minimum=0):
    """Confirmed fee-asset UTXOs the wallet can sign for, largest first."""
    out = []
    for u in rpc.call("listunspent", 1, 9999999):
        if u.get("asset") != chain.fee_asset or not u.get("spendable", True):
            continue
        if u.get("amountblinder", "0" * 64) != "0" * 64:
            continue  # blinded: bigger, and pointless for a fee-floor wall
        atoms = to_atoms(u["amount"])
        if atoms < minimum:
            continue
        out.append({"txid": u["txid"], "vout": u["vout"], "atoms": atoms, "address": u["address"]})
    out.sort(key=lambda u: -u["atoms"])
    return out


def do_fanout(rpc, chain, args):
    """Split the budget into a working set of independent, unblinded UTXOs.

    Independent matters more than it looks: unconfirmed transactions may only
    chain 25 deep (DEFAULT_ANCESTOR_LIMIT), so a wall of backlog several blocks
    high needs many separate roots, not one long chain.
    """
    balances = rpc.call("getbalances")["mine"]["trusted"]
    have = to_atoms(balances.get(chain.fee_asset_label, balances.get(chain.fee_asset, 0)))
    budget = to_atoms(args.budget) if args.budget else have * 9 // 10
    if budget > have:
        sys.exit("budget %s exceeds the wallet's %s %s" % (amount(budget), amount(have), chain.fee_asset_label))
    n = args.utxos
    each = budget // n
    if each < 1000:
        sys.exit("%s %s over %d UTXOs is %s each -- too small to pay a fee; raise --budget or lower --utxos"
                 % (amount(budget), chain.fee_asset_label, n, amount(each)))

    print("Fanning %s %s into %d UTXOs of %s each."
          % (amount(budget), chain.fee_asset_label, n, amount(each)))
    made = 0
    while made < n and not _shutdown.is_set():
        # A transaction is standard only up to 100 kvB, and must also fit the
        # block: keep each fan-out well inside both.
        batch = min(args.fanout_batch, n - made)
        outputs = {}
        for _ in range(batch):
            addr = unconfidential(rpc, rpc.call("getnewaddress", "spam"))
            outputs[addr] = amount(each)
        txid = rpc.call("sendmany", dummy="", amounts=outputs,
                        output_assets={a: chain.fee_asset for a in outputs},
                        fee_asset=chain.fee_asset)
        made += batch
        print("  %s  (%d/%d)" % (txid, made, n))
        # Wait for it before building the next one, so the fan-outs do not chain
        # into each other and hit the ancestor limit on the way in.
        deadline = time.time() + args.fanout_timeout
        while time.time() < deadline and not _shutdown.is_set():
            if rpc.call("gettransaction", txid).get("confirmations", 0) >= 1:
                break
            time.sleep(2)
        else:
            print("  (still unconfirmed after %ds -- is the chain advancing? on regtest nothing "
                  "produces blocks unless something calls generatetoaddress)" % args.fanout_timeout)
    ready = len(spendable(rpc, chain, minimum=each // 2))
    if not ready:
        sys.exit("Working set is EMPTY: the fan-out never confirmed, so --run has nothing to spend. "
                 "Let the chain catch up and re-run --fanout.")
    print("Working set ready: %d UTXOs." % ready)


# --------------------------------------------------------------------------- run


class Spammer:
    def __init__(self, rpc, chain, args, rate):
        self.rpc, self.chain, self.args, self.rate = rpc, chain, args, rate
        self.pool = queue.Queue()
        self.calibrated_vsize = None
        self.lock = threading.Lock()
        self.sent = 0
        self.spent_atoms = 0
        self.errors = {}
        self.budget_atoms = to_atoms(args.budget) if args.budget else None
        self.block_vsize = chain.usable_block_vsize(args.block_vsize)
        self.target_bytes = int(self.block_vsize * args.target_backlog_blocks)

    # -- one transaction ----------------------------------------------------

    def build(self, utxo, rate):
        """1-in, 1-out + explicit fee, spending back to the input's own address.

        Same address in and out keeps the working set stable (one UTXO in, one
        UTXO out, shrinking by exactly the fee) and keeps the transaction the
        smallest shape the chain will relay, which is what makes a given budget
        buy the most block space.
        """
        vsize = self.calibrated_vsize or self.args.assumed_vsize
        fee = self.chain.atoms_for_value(rate * (vsize + 1))  # +1 vB: signature length varies
        change = utxo["atoms"] - fee
        if change < self.args.dust:
            return None, fee
        outputs = [
            {utxo["address"]: amount(change), "asset": self.chain.fee_asset},
            {"fee": amount(fee), "fee_asset": self.chain.fee_asset},
        ]
        raw = self.rpc.call("createrawtransaction",
                            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
                            outputs, 0, self.args.rbf)
        signed = self.rpc.call("signrawtransactionwithwallet", raw)
        if not signed.get("complete"):
            raise RPCError(-1, "signing failed: %s" % signed.get("errors"))
        return signed["hex"], fee

    def calibrate(self, hexstr):
        if self.calibrated_vsize is not None:
            return
        d = self.rpc.call("decoderawtransaction", hexstr, wallet_path=False)
        # The mempool ranks by the DISCOUNT size (feeassets.cpp), so that, not
        # vsize, is the size a fee rate has to be computed against.
        with self.lock:
            if self.calibrated_vsize is None:
                self.calibrated_vsize = int(d.get("discountvsize", d["vsize"]))
                print("Calibrated: one spam transaction is %d vB (discount size)." % self.calibrated_vsize)

    def worker(self):
        while not _shutdown.is_set():
            try:
                utxo = self.pool.get(timeout=1)
            except queue.Empty:
                continue
            try:
                if self.paused():
                    self.pool.put(utxo)
                    time.sleep(0.25)
                    continue
                # A ladder rather than a single price: a wall where every
                # transaction pays exactly the same rate answers "did I beat the
                # floor" but never "how far above it do I have to bid", which is
                # the question RBF and CPFP are really asking.
                rate = int(self.rate * random.uniform(1.0, self.args.spread))
                hexstr, fee = self.build(utxo, rate)
                if hexstr is None:
                    continue  # UTXO worn down to dust; drop it
                self.calibrate(hexstr)
                txid = self.rpc.call("sendrawtransaction", hexstr, 0, wallet_path=False)
                with self.lock:
                    self.sent += 1
                    self.spent_atoms += fee
                self.pool.put({"txid": txid, "vout": 0, "atoms": utxo["atoms"] - fee,
                               "address": utxo["address"]})
            except RPCError as e:
                with self.lock:
                    self.errors[e.message[:70]] = self.errors.get(e.message[:70], 0) + 1
                # too-long-mempool-chain and friends: this UTXO is stuck until a
                # block confirms its chain, so park it and come back later.
                time.sleep(1.0)
                self.pool.put(utxo)
            finally:
                self.pool.task_done()

    # -- throttle -----------------------------------------------------------

    def paused(self):
        return self._paused

    def run(self):
        args, chain = self.args, self.chain
        for u in spendable(self.rpc, chain, minimum=self.args.dust * 4):
            self.pool.put(u)
        if self.pool.qsize() < 2:
            sys.exit("only %d usable UTXO(s): run --fanout first" % self.pool.qsize())
        how = ("the node's own getmempoolcongestion" if chain.congestion_rpc
               else "%s vB per block (--block-vsize)" % "{:,}".format(self.block_vsize))
        print("Working set: %d UTXOs. Holding %.1f blocks of backlog, measured by %s. Rate: %s/vB"
              % (self.pool.qsize(), args.target_backlog_blocks, how, fmt_usd(self.rate)))
        if self.budget_atoms:
            print("Budget: %s %s." % (amount(self.budget_atoms), chain.fee_asset_label))

        self._paused = False
        threads = [threading.Thread(target=self.worker, daemon=True) for _ in range(args.threads)]
        for t in threads:
            t.start()

        end = time.time() + parse_duration(args.duration) if args.duration else None
        last_report = 0
        try:
            while not _shutdown.is_set():
                floor = None
                if chain.congestion_rpc:
                    c = self.rpc.call("getmempoolcongestion", wallet_path=False)
                    backlog, blocks = int(c["bytes"]), float(c["backlog_blocks"])
                    mp_size = int(c["size"])
                    # What the wall is actually charging, straight from the
                    # producer's own ordering: the number the wallet under test
                    # has to beat.
                    if c["next_block_full"]:
                        floor = int(c["next_block_min_atoms_per_kvb"]) / 1000.0
                else:
                    mp = self.rpc.call("getmempoolinfo", wallet_path=False)
                    backlog, mp_size = int(mp["bytes"]), int(mp["size"])
                    blocks = backlog / self.block_vsize
                # Closed loop on the backlog itself. Filling as fast as possible
                # would just burn the budget into a mempool nobody mines; what
                # the test needs is a queue that stays a few blocks deep for the
                # whole run.
                self._paused = blocks > args.target_backlog_blocks
                with self.lock:
                    sent, spent = self.sent, self.spent_atoms
                if self.budget_atoms and spent >= self.budget_atoms:
                    print("\nBudget spent (%s %s). Stopping." % (amount(spent), chain.fee_asset_label))
                    break
                if end and time.time() >= end:
                    print("\nDuration reached. Stopping.")
                    break
                if time.time() - last_report >= args.report_seconds:
                    last_report = time.time()
                    left = ""
                    if self.budget_atoms:
                        left = "  budget left %s" % amount(self.budget_atoms - spent)
                    print("  backlog %7s vB (%4.1f blocks)  mempool %5d tx  floor %s  sent %6d  spent %s %s%s%s"
                          % ("{:,}".format(backlog), blocks, mp_size,
                             ("%s/vB" % fmt_usd(floor)) if floor else "  (not full)", sent,
                             amount(spent), chain.fee_asset_label, left,
                             "  [holding]" if self._paused else ""))
                    if self.errors:
                        with self.lock:
                            top = sorted(self.errors.items(), key=lambda kv: -kv[1])[:2]
                            self.errors = {}
                        for msg, n in top:
                            print("      %dx %s" % (n, msg))
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            _shutdown.set()
            for t in threads:
                t.join(timeout=5)
            print("Sent %d transactions, spent %s %s."
                  % (self.sent, amount(self.spent_atoms), chain.fee_asset_label))


# ------------------------------------------------------------------------- sweep


def do_sweep(rpc, chain, args):
    """Put the working set back into one output, so the next run starts clean."""
    utxos = spendable(rpc, chain)
    if not utxos:
        print("nothing to sweep")
        return
    addr = unconfidential(rpc, rpc.call("getnewaddress", "spam-sweep"))
    per_tx = args.fanout_batch
    for i in range(0, len(utxos), per_tx):
        chunk = utxos[i:i + per_tx]
        total = sum(u["atoms"] for u in chunk)
        # Consolidation is cheap in value terms; pay the relay floor, generously.
        fee = chain.atoms_for_value(max(chain.relay_min_per_kvb, 1) * (len(chunk) * 70 + 200) // 1000 * 4)
        fee = max(fee, 1000)
        if total <= fee:
            continue
        raw = rpc.call("createrawtransaction",
                       [{"txid": u["txid"], "vout": u["vout"]} for u in chunk],
                       [{addr: amount(total - fee), "asset": chain.fee_asset},
                        {"fee": amount(fee), "fee_asset": chain.fee_asset}])
        signed = rpc.call("signrawtransactionwithwallet", raw)
        txid = rpc.call("sendrawtransaction", signed["hex"], 0, wallet_path=False)
        print("  swept %d inputs -> %s" % (len(chunk), txid))


# -------------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    con = ap.add_argument_group("node")
    con.add_argument("--rpchost", default="127.0.0.1")
    con.add_argument("--rpcport", type=int, default=0, help="default: 18776 (test), 7041 (main), 18884 (regtest)")
    con.add_argument("--rpcuser", default="")
    con.add_argument("--rpcpassword", default="")
    con.add_argument("--cookie", default="", help="path to the node's .cookie")
    con.add_argument("--datadir", default="", help="node datadir, to find the cookie")
    con.add_argument("--chain-subdir", default="", help="datadir subdirectory of the chain (default: guess from --rpcport)")
    con.add_argument("--wallet", default="", help="wallet name, if the node has more than one loaded")

    fee = ap.add_argument_group("fee floor to manufacture")
    fee.add_argument("--rate-cents-per-vb", type=float, help="e.g. 0.05 -- US cents of value per vB")
    fee.add_argument("--rate-usd-per-vb", type=float, help="e.g. 0.0005 -- reference units per vB")
    fee.add_argument("--rate-atoms-per-kvb", type=float, help="reference atoms per kvB, as -minrelaytxfee counts them")
    fee.add_argument("--spread", type=float, default=1.5,
                     help="spread rates over [rate, rate*spread] so the wall has a gradient (default 1.5)")
    fee.add_argument("--fee-asset", default="", help="label or id of the asset fees are paid in (default: policy asset)")

    lim = ap.add_argument_group("limits")
    lim.add_argument("--duration", default="", help="stop after e.g. 90m, 4h")
    lim.add_argument("--budget", type=float, default=0, help="hard cap on fees, in whole units of the fee asset")
    lim.add_argument("--target-backlog-blocks", type=float, default=3.0,
                     help="how many blocks of queue to hold (default 3)")
    lim.add_argument("--block-vsize", type=int, default=0, help="override the block ceiling in vB")
    lim.add_argument("--block-seconds", type=int, default=60, help="block spacing, for the cost projection")

    tune = ap.add_argument_group("tuning")
    tune.add_argument("--utxos", type=int, default=1000, help="size of the working set (--fanout)")
    tune.add_argument("--fanout-batch", type=int, default=400, help="outputs per fan-out transaction")
    tune.add_argument("--fanout-timeout", type=int, default=180)
    tune.add_argument("--threads", type=int, default=4)
    tune.add_argument("--assumed-vsize", type=int, default=260, help="size guess for the first transaction only")
    tune.add_argument("--dust", type=int, default=5000, help="atoms below which a UTXO is abandoned")
    tune.add_argument("--rbf", action="store_true", help="make the spam itself replaceable (default: not)")
    tune.add_argument("--report-seconds", type=int, default=10)

    act = ap.add_argument_group("what to do")
    act.add_argument("--plan", action="store_true", help="price the run and check the budget; touch nothing")
    act.add_argument("--fanout", action="store_true", help="build the working set of UTXOs")
    act.add_argument("--run", action="store_true", help="hold the fee floor")
    act.add_argument("--sweep", action="store_true", help="consolidate the working set back")
    args = ap.parse_args()

    if not (args.plan or args.fanout or args.run or args.sweep):
        ap.error("pick one of --plan / --fanout / --run / --sweep")

    args.rpcport_given = bool(args.rpcport)
    if not args.rpcport:
        args.rpcport = 18776
    if not args.chain_subdir:
        args.chain_subdir = {7041: "sequentia", 18776: "testnet3", 18884: "elementsregtest"}.get(args.rpcport, "testnet3")

    user, pw = read_auth(args)
    rpc = RPC(args.rpchost, args.rpcport, user, pw, args.wallet or None)
    try:
        chain = Chain(rpc, args.fee_asset)
    except RPCError as e:
        sys.exit("cannot talk to the node: %s" % e)

    signal.signal(signal.SIGINT, lambda *_: _shutdown.set())

    rate = resolve_rate(args, chain)
    if args.plan:
        sys.exit(do_plan(rpc, chain, args, rate))
    if args.fanout:
        do_fanout(rpc, chain, args)
    if args.run:
        if rate is None:
            sys.exit("--run needs a --rate-*")
        if rate * 1000 < chain.relay_min_per_kvb:
            sys.exit("target rate is below this node's relay floor (%d atoms/kvB): nothing would be accepted"
                     % chain.relay_min_per_kvb)
        Spammer(rpc, chain, args, rate).run()
    if args.sweep:
        do_sweep(rpc, chain, args)


if __name__ == "__main__":
    main()
