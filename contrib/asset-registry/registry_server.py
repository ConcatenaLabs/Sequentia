#!/usr/bin/env python3
# Copyright (c) 2026 The Sequentia developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sequentia asset registry server.

A self-hosted Asset Registry: the {asset id -> [domain, ticker, name,
precision, verified]} index that nodes (-assetregistryurl), price servers
(registry_url), explorers and wallets read to turn 64-hex asset ids into
human names — and to know which entries the registrar has VERIFIED.

It serves three purposes at once:

  MIRROR    it periodically syncs an upstream registry (another operator's)
            and keeps the last good copy, so consumers pointed here keep
            working even when the upstream is down.
  REGISTRAR the operator can add, edit, verify and delete entries from the
            admin web UI (password-protected, exactly like the price
            server's). Local edits override the upstream and survive syncs.
  API       the merged result is served at /registry/index.minimal.json in
            the exact upstream format, so this URL is a drop-in replacement
            anywhere a registry URL is configured.

Stdlib only; no external dependencies.

Usage:
    registry_server.py --config config.json [--port N] [--host H]
    registry_server.py --config config.json --set-password
"""

import argparse
import collections
import hashlib
import hmac
import html
import http.server
import ipaddress
import json
import logging
import os
import re
import secrets
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

log = logging.getLogger("asset-registry")

DEFAULT_UPSTREAM = "https://sequentiatestnet.com/registry/index.minimal.json"
DEFAULT_EXPLORER = "https://sequentiatestnet.com/api"
DEFAULT_NAME = "Sequentia Asset Registry"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
PUBKEY_HEX = re.compile(r"^[0-9a-f]{66}$")   # 33-byte compressed
XONLY_HEX = re.compile(r"^[0-9a-f]{64}$")    # 32-byte x-only (OpenAMP enclave keys)
TICKER_RE = re.compile(r"^[A-Za-z0-9.\-]{1,12}$")
DOMAIN_RE = re.compile(r"^(?=.{1,253}$)([a-z0-9](-?[a-z0-9])*\.)+[a-z]{2,}$", re.I)
PLACEHOLDER_PUBKEY = re.compile(r"^(?:[0-9a-f]{2})?0{64}$", re.I)


def http_get_json(url, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": "sequentia-asset-registry/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def _atomic_write(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
    os.replace(tmp, path)


def _entry_ok(e):
    """A registry entry is [domain, ticker, name, precision, verified]."""
    return (isinstance(e, list) and len(e) >= 5
            and isinstance(e[0], str) and isinstance(e[1], str) and isinstance(e[2], str)
            and isinstance(e[3], int) and 0 <= e[3] <= 8 and e[4] in (0, 1))


# ---------------------------------------------------------------------------
# Registration API: earning `verified` instead of being granted it.
#
# The admin UI's verified checkbox is the operator's word. This is the other
# way, the one that needs no trust in the operator at all:
#
#   1. the asset's metadata lives in a "contract" whose SHA256 the issuance
#      committed on-chain, and from which the asset id is derived, so nobody can
#      register metadata for someone else's asset;
#   2. the issuer publishes a proof at their contract's domain, so nobody can
#      claim a domain they do not control.
#
# Every rule below deliberately mirrors the reference implementation
# (sequentia-registry, server.js). An asset accepted here must be an asset that
# registry would accept and vice versa, or "run your own registry" means running
# an incompatible one.
# ---------------------------------------------------------------------------

class ApiError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


def _canonical_json(v):
    """Canonical JSON: keys sorted, no insignificant whitespace.

    json.dumps with sort_keys and tight separators matches JavaScript's
    JSON.stringify for the values a contract may carry (strings, integers).
    ensure_ascii must stay off: JSON.stringify emits non-ASCII raw, and one
    escaped character would be a different hash, hence a different asset id.
    """
    return json.dumps(v, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _contract_hash(contract):
    return hashlib.sha256(_canonical_json(contract).encode("utf-8")).hexdigest()


# --- asset id derivation ---------------------------------------------------
# Elements/Sequentia derive the asset id from (issuance prevout, contract_hash)
# through a "fast" merkle tree whose node hash is the SHA256 *midstate* of the
# two 32-byte children: one 64-byte block, no length padding, no finalisation.
# hashlib cannot express that, so the compression function is spelled out here.
# See src/issuance.cpp (GenerateAssetEntropy/CalculateAsset) and
# src/primitives/txwitness.cpp (MerkleHash_Sha256Midstate).

_SHA256_K = (
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
)
_SHA256_IV = (0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19)
_M32 = 0xffffffff


def _rotr(x, n):
    return ((x >> n) | (x << (32 - n))) & _M32


def _sha256_midstate(block64):
    """Compress one 64-byte block and return the raw chaining state (32 bytes)."""
    assert len(block64) == 64
    w = list(struct.unpack(">16I", block64)) + [0] * 48
    for i in range(16, 64):
        s0 = _rotr(w[i - 15], 7) ^ _rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)
        s1 = _rotr(w[i - 2], 17) ^ _rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & _M32
    a, b, c, d, e, f, g, h = _SHA256_IV
    for i in range(64):
        S1 = _rotr(e, 6) ^ _rotr(e, 11) ^ _rotr(e, 25)
        ch = (e & f) ^ ((~e & _M32) & g)
        t1 = (h + S1 + ch + _SHA256_K[i] + w[i]) & _M32
        S0 = _rotr(a, 2) ^ _rotr(a, 13) ^ _rotr(a, 22)
        maj = (a & b) ^ (a & c) ^ (b & c)
        t2 = (S0 + maj) & _M32
        h, g, f, e, d, c, b, a = g, f, e, (d + t1) & _M32, c, b, a, (t1 + t2) & _M32
    return struct.pack(">8I", *[(iv + v) & _M32 for iv, v in zip(_SHA256_IV, (a, b, c, d, e, f, g, h))])


def _merkle_node(left, right):
    return _sha256_midstate(left + right)


def _derive_asset_id(prevout_txid, prevout_vout, contract_hash_hex):
    """The asset id the chain would derive, or None if the inputs are malformed.

    leaf    = SHA256d(COutPoint), COutPoint = txid(internal order) || vout(LE32)
    entropy = merkle(leaf, contract_hash)
    asset   = merkle(entropy, 0^32), printed reversed like every uint256
    """
    if not HEX64.match(prevout_txid or "") or not HEX64.match(contract_hash_hex or ""):
        return None
    if not isinstance(prevout_vout, int) or not 0 <= prevout_vout <= 0xffffffff:
        return None
    # explorers print txids reversed; COutPoint serialises the internal order
    outpoint = bytes.fromhex(prevout_txid)[::-1] + struct.pack("<I", prevout_vout)
    leaf = hashlib.sha256(hashlib.sha256(outpoint).digest()).digest()
    entropy = _merkle_node(leaf, bytes.fromhex(contract_hash_hex))
    asset_internal = _merkle_node(entropy, b"\x00" * 32)
    return asset_internal[::-1].hex()


# --- contract validation ---------------------------------------------------

def _validate_contract(c):
    """Liquid-compatible shape: {name, ticker, precision, entity:{domain},
    issuer_pubkey, version}. Returns a list of problems, empty if good."""
    errs = []
    if not isinstance(c, dict):
        return ["contract must be an object"]
    name = c.get("name")
    if not isinstance(name, str) or not 1 <= len(name) <= 255:
        errs.append("name: 1..255 chars")
    if not isinstance(c.get("ticker"), str) or not TICKER_RE.match(c["ticker"]):
        errs.append("ticker: 1..12 of [A-Za-z0-9.-]")
    if not isinstance(c.get("precision"), int) or isinstance(c.get("precision"), bool) \
            or not 0 <= c["precision"] <= 8:
        errs.append("precision: integer 0..8")
    entity = c.get("entity")
    if not isinstance(entity, dict) or not isinstance(entity.get("domain"), str) \
            or not DOMAIN_RE.match(entity["domain"]):
        errs.append("entity.domain: valid domain")
    else:
        for k in entity:
            if k not in ("domain", "issuer"):
                errs.append("unexpected entity field: %s" % k)
        if "issuer" in entity and not isinstance(entity["issuer"], str):
            errs.append("entity.issuer: string")
    pk = c.get("issuer_pubkey")
    if not isinstance(pk, str) or not (PUBKEY_HEX.match(pk) or XONLY_HEX.match(pk)):
        errs.append("issuer_pubkey: 33-byte compressed or 32-byte x-only hex")
    elif PLACEHOLDER_PUBKEY.match(pk):
        errs.append("issuer_pubkey: must not have an all-zeros X coordinate (placeholder)")
    if c.get("version") != 0:
        errs.append("version: must be 0")
    # Unknown top-level keys are refused so the canonical hash is well defined.
    allowed = {"name", "ticker", "precision", "entity", "issuer_pubkey", "version", "openamp", "operator"}
    for k in c:
        if k not in allowed:
            errs.append("unexpected field: %s" % k)
    return errs


# --- fetching --------------------------------------------------------------

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Refuse to follow redirects.

    The reference registry uses a bare Node http.get, which does not follow
    them, so a domain that answers the proof with a 301 (to www, say) fails
    there. Following them here would accept assets that registry rejects.
    """
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _assert_public_host(host):
    """Refuse to fetch loopback/private/link-local/metadata addresses, so an
    issuer domain cannot point this server at things behind its own firewall."""
    try:
        infos = socket.getaddrinfo(host, 443, proto=socket.IPPROTO_TCP)
    except OSError:
        raise ApiError(400, "dns lookup failed for %s" % host)
    if not infos:
        raise ApiError(400, "dns lookup empty for %s" % host)
    for info in infos:
        try:
            ip = ipaddress.ip_address(info[4][0])
        except ValueError:
            raise ApiError(400, "unparseable address for %s" % host)
        if not ip.is_global:
            raise ApiError(400, "%s resolves to a non-public address" % host)


def _proof_text(domain, asset_id):
    return "Authorize linking the domain name %s to the Sequentia asset %s" % (domain, asset_id)


def _verify_domain_proof(domain, asset_id, timeout):
    url = "https://%s/.well-known/sequentia-asset-proof-%s" % (domain, asset_id)
    _assert_public_host(domain)
    opener = urllib.request.build_opener(_NoRedirect)
    req = urllib.request.Request(url, headers={"User-Agent": "sequentia-asset-registry/0.1"})
    try:
        with opener.open(req, timeout=timeout) as resp:
            ctype = resp.headers.get_content_type()
            body = resp.read(1_000_000).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raise ApiError(400, "domain proof not found at %s (HTTP %d)" % (url, e.code))
    except Exception as e:
        raise ApiError(400, "domain proof fetch failed: %s" % e)
    # text/plain is required so nobody can smuggle the line inside an HTML page
    # (a user-content page, say) they do not actually control.
    if ctype != "text/plain":
        raise ApiError(400, "domain proof at %s must be served as text/plain (got '%s')" % (url, ctype or "none"))
    # The whole body must BE the line, not merely contain it: a substring match
    # would let attacker-influenced content pass.
    if body.strip() != _proof_text(domain, asset_id):
        raise ApiError(400, "domain proof at %s must contain exactly the authorization line and nothing else" % url)
    return url


def _onchain_contract(asset_id, explorer, timeout):
    """{contract_hash (natural order), prevout_txid, prevout_vout, issuance_txid}."""
    base = explorer.rstrip("/")
    try:
        asset = http_get_json("%s/asset/%s" % (base, asset_id), timeout)
    except Exception as e:
        raise ApiError(400, "asset %s not found on chain (%s)" % (asset_id, e))
    itx = asset.get("issuance_txin") or {}
    if not itx.get("txid"):
        raise ApiError(400, "asset has no issuance on chain")
    try:
        tx = http_get_json("%s/tx/%s" % (base, itx["txid"]), timeout)
    except Exception as e:
        raise ApiError(400, "issuance tx not found (%s)" % e)
    try:
        vin = tx["vin"][itx["vin"]]
    except Exception:
        raise ApiError(400, "issuance input not found in issuance tx")
    iss = vin.get("issuance")
    if not iss:
        raise ApiError(400, "issuance input not found in issuance tx")
    if iss.get("is_reissuance"):
        raise ApiError(400, "issuance input is a reissuance, not the initial issuance")
    ch = iss.get("contract_hash") or ""
    if not HEX64.match(ch):
        raise ApiError(400, "issuance input has no valid contract_hash on chain")
    # Explorers print the contract_hash as a uint256, i.e. reversed. The
    # commitment is SHA256(canonical contract) in natural order, so put it back.
    return {
        "contract_hash": bytes.fromhex(ch)[::-1].hex(),
        "prevout_txid": vin.get("txid"),
        "prevout_vout": vin.get("vout"),
        "issuance_txid": itx["txid"],
    }


def register_asset(reg, asset_id, contract):
    """Verify an asset against the chain and its issuer's domain, then record it.

    Raises ApiError with a message meant to be read by whoever is trying to
    register: every rejection here is something they can go and fix.
    """
    asset_id = str(asset_id or "").lower()
    if not HEX64.match(asset_id):
        raise ApiError(400, "asset_id: 64-hex")
    errs = _validate_contract(contract)
    if errs:
        raise ApiError(400, "invalid contract: " + "; ".join(errs))

    ch = _contract_hash(contract)
    ticker = contract["ticker"]
    domain = contract["entity"]["domain"]

    # Tickers are first-come and case-insensitive; refuse to let a later
    # registration squat one somebody already holds.
    for aid, e in reg.merged().items():
        if aid != asset_id and isinstance(e, list) and len(e) >= 2 \
                and str(e[1]).lower() == ticker.lower():
            raise ApiError(409, "ticker '%s' is already registered to a different asset" % ticker)

    explorer = reg.cfg.get("explorer_url", DEFAULT_EXPLORER)
    if not explorer:
        raise ApiError(503, "this registry has no explorer configured, so it cannot verify assets "
                            "against the chain; the operator must set explorer_url")
    timeout = int(reg.cfg.get("proof_timeout", 15) or 15)

    oc = _onchain_contract(asset_id, explorer, timeout)
    if oc["contract_hash"] != ch:
        raise ApiError(400, "contract does not match on-chain commitment: on-chain contract_hash=%s, "
                            "SHA256(contract)=%s. The asset must have been issued with this exact contract."
                            % (oc["contract_hash"], ch))
    # Re-derive the id from the issuance as well, so a wrong or hostile explorer
    # answer cannot decouple the asset id from the contract we just checked.
    derived = _derive_asset_id(oc["prevout_txid"], oc["prevout_vout"], oc["contract_hash"])
    if derived is None:
        raise ApiError(400, "could not re-derive asset id from issuance prevout (incomplete chain data)")
    if derived != asset_id:
        raise ApiError(400, "asset id does not match its on-chain derivation: derived=%s, submitted=%s"
                            % (derived, asset_id))

    proof_url = None
    if reg.cfg.get("require_domain_proof", True):
        proof_url = _verify_domain_proof(domain, asset_id, timeout)

    reg.set_entry(asset_id, [domain, ticker, contract["name"], int(contract["precision"]), 1])
    log.info("registered %s (%s) for %s", asset_id, ticker, domain)
    return {
        "asset_id": asset_id,
        "contract": contract,
        "contract_hash": ch,
        "issuance_txid": oc["issuance_txid"],
        "verified": True,
        "verified_chain": True,
        "verified_domain": proof_url is not None,
        "verified_by": "issuer",
        "proof_url": proof_url,
    }


class Registry:
    """The registry state: an upstream cache plus local overrides.

    merged = upstream overlaid by overrides["set"], minus overrides["deleted"].
    Local edits always win over the upstream, and a sync never clobbers them:
    the registrar's word is final on their own registry."""

    def __init__(self, cfg, config_path):
        self.cfg = cfg
        self.config_path = config_path
        self._lock = threading.Lock()
        d = os.path.dirname(os.path.abspath(config_path))
        self.cache_path = os.path.join(d, "upstream-cache.json")
        self.overrides_path = os.path.join(d, "overrides.json")
        self.upstream = {}
        self.upstream_ts = None
        self.upstream_ok = None    # last sync outcome: True/False/None(never)
        self.upstream_err = ""
        self.overrides = {"set": {}, "deleted": []}
        try:
            c = json.load(open(self.cache_path))
            self.upstream = c.get("index", {})
            self.upstream_ts = c.get("fetched_at")
        except Exception:
            pass
        try:
            o = json.load(open(self.overrides_path))
            self.overrides = {"set": o.get("set", {}), "deleted": o.get("deleted", [])}
        except Exception:
            pass

    # ---- config ----
    def save_config(self):
        _atomic_write(self.config_path, self.cfg)

    def ui(self):
        return self.cfg.get("ui", {})

    def name(self):
        return self.cfg.get("registry_name", DEFAULT_NAME)

    # ---- sync ----
    def sync(self):
        url = self.cfg.get("upstream_url", DEFAULT_UPSTREAM)
        if not url:
            self.upstream_ok = None
            return
        try:
            raw = http_get_json(url, self.cfg.get("upstream_timeout", 15))
            index = {}
            for aid, e in raw.items():
                aid = str(aid).lower()
                if HEX64.match(aid) and _entry_ok(e):
                    index[aid] = [e[0], e[1], e[2], int(e[3]), int(e[4])]
            with self._lock:
                self.upstream = index
                self.upstream_ts = time.time()
                self.upstream_ok = True
                self.upstream_err = ""
            _atomic_write(self.cache_path, {"index": index, "fetched_at": self.upstream_ts,
                                            "source": url})
            log.info("synced %d entries from %s", len(index), url)
        except Exception as e:
            with self._lock:
                self.upstream_ok = False
                self.upstream_err = str(e)
            log.warning("upstream sync failed (%s); serving the last good copy (%d entries)",
                        e, len(self.upstream))

    def sync_loop(self):
        while True:
            self.sync()
            time.sleep(max(30, int(self.cfg.get("sync_interval_secs", 300))))

    # ---- data ----
    def merged(self):
        with self._lock:
            out = dict(self.upstream)
            out.update(self.overrides["set"])
            for aid in self.overrides["deleted"]:
                out.pop(aid, None)
            return out

    def provenance(self, aid):
        """'local' (not upstream), 'edited' (overrides an upstream entry) or 'upstream'."""
        if aid in self.overrides["set"]:
            return "edited" if aid in self.upstream else "local"
        return "upstream"

    def set_entry(self, aid, entry):
        with self._lock:
            self.overrides["set"][aid] = entry
            if aid in self.overrides["deleted"]:
                self.overrides["deleted"].remove(aid)
            _atomic_write(self.overrides_path, self.overrides)

    def delete_entry(self, aid):
        with self._lock:
            self.overrides["set"].pop(aid, None)
            if aid in self.upstream and aid not in self.overrides["deleted"]:
                self.overrides["deleted"].append(aid)   # tombstone an upstream entry
            _atomic_write(self.overrides_path, self.overrides)

    def restore_entry(self, aid):
        """Drop the local override/tombstone so the upstream value shows again."""
        with self._lock:
            self.overrides["set"].pop(aid, None)
            if aid in self.overrides["deleted"]:
                self.overrides["deleted"].remove(aid)
            _atomic_write(self.overrides_path, self.overrides)


# ---------------------------------------------------------------------------
# Auth & rate limiting (same model as the price server).
# ---------------------------------------------------------------------------

def _hash_password(pw, iterations=200_000):
    salt = secrets.token_bytes(16)
    dk = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, iterations)
    return "pbkdf2$%d$%s$%s" % (iterations, salt.hex(), dk.hex())


def _check_password(stored, pw):
    try:
        _scheme, it, salt, want = stored.split("$")
        dk = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), int(it))
        return hmac.compare_digest(dk.hex(), want)
    except Exception:
        return False


class _RateLimiter:
    def __init__(self):
        self._hits = {}
        self._lock = threading.Lock()

    def allow(self, ip, per_min):
        now = time.time()
        with self._lock:
            q = self._hits.setdefault(ip, collections.deque())
            while q and q[0] < now - 60:
                q.popleft()
            if len(q) >= per_min:
                return False
            q.append(now)
            if len(self._hits) > 10_000:
                self._hits = {k: v for k, v in self._hits.items() if v}
            return True


# ---------------------------------------------------------------------------
# Pages. Sequentia yellow-on-black, same look as the price server.
# ---------------------------------------------------------------------------

_CSS = """
:root{--bg:#0b0b0d;--panel:#141417;--panel2:#191920;--line:#26262c;--text:#f2f0ea;
--muted:#9b988e;--faint:#6d6a62;--accent:#f5b301;--accent-ink:#1a1400;
--good:#3ecf7a;--good-bg:rgba(62,207,122,.12);--warn:#ffb84d;--warn-bg:rgba(255,160,50,.12);
--bad:#ff6b6b;--bad-bg:rgba(255,90,90,.12);--mono:ui-monospace,'Cascadia Mono',Consolas,Menlo,monospace}
html,body{background:var(--bg);color:var(--text);margin:0}
body{font-family:'Segoe UI',system-ui,-apple-system,sans-serif;font-size:15px;line-height:1.45}
.wrap{max-width:1080px;margin:0 auto;padding:20px 18px 48px}
header{display:flex;align-items:center;gap:14px;padding:6px 0 14px;flex-wrap:wrap}
.mark{width:42px;height:42px;border-radius:8px;background:var(--accent);color:var(--accent-ink);
display:flex;align-items:center;justify-content:center;font-weight:800;font-size:1.05rem;font-family:var(--mono)}
.htitle h1{font-size:1.2rem;margin:0;font-weight:650}
.htitle .sub{color:var(--muted);font-size:.8rem;margin-top:2px}
.hchips{margin-left:auto;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.chip{font-size:.68rem;font-weight:700;letter-spacing:.09em;text-transform:uppercase;
padding:4px 10px;border-radius:3px;border:1px solid var(--line);color:var(--muted)}
.chip.live{border-color:var(--good);color:var(--good)}
.chip.warn{border-color:var(--warn);color:var(--warn)}
.chip a{color:inherit;text-decoration:none}
.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;overflow:hidden;margin-bottom:14px}
.card h2{font-size:.72rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--accent);
margin:0;padding:12px 16px 10px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:10px}
.card h2 .right{margin-left:auto;text-transform:none;letter-spacing:0;color:var(--faint);font-weight:400}
table{width:100%;border-collapse:collapse;font-size:.88rem}
th{font-size:.68rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--faint);
text-align:left;padding:8px 14px;border-bottom:1px solid var(--line);white-space:nowrap}
td{padding:8px 14px;border-bottom:1px solid rgba(255,255,255,.04);vertical-align:middle}
tr:last-child td{border-bottom:none}
.num{font-family:var(--mono);font-variant-numeric:tabular-nums}
.mut{color:var(--muted);font-weight:400}
.mono{font-family:var(--mono);font-size:.8rem}
.pill{display:inline-block;font-size:.7rem;font-weight:700;padding:2px 8px;border-radius:10px}
.pill.good{background:var(--good-bg);color:var(--good)}
.pill.warn{background:var(--warn-bg);color:var(--warn)}
.pill.bad{background:var(--bad-bg);color:var(--bad)}
.kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin-bottom:14px}
@media(max-width:820px){.kpis{grid-template-columns:repeat(2,1fr)}}
.kpi{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px 14px}
.kpi .l{color:var(--faint);font-size:.7rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase}
.kpi .v{font-family:var(--mono);font-size:1.25rem;font-weight:700;margin-top:4px}
.kpi .s{color:var(--muted);font-size:.74rem;margin-top:2px}
.frm{padding:14px 16px;display:grid;gap:12px}
.frow{display:grid;grid-template-columns:220px 1fr;gap:12px;align-items:center}
@media(max-width:640px){.frow{grid-template-columns:1fr}}
.frow label{color:var(--muted);font-size:.88rem}
.hint{color:var(--faint);font-size:.75rem;line-height:1.4}
input:not([type=checkbox]):not([type=radio]),select{
background:var(--panel2);border:1px solid var(--line);border-radius:4px;color:var(--text);
font:inherit;font-size:.88rem;padding:8px 10px;width:100%;box-sizing:border-box}
input:focus,select:focus{outline:none;border-color:var(--accent)}
input[type=checkbox]{accent-color:var(--accent);width:16px;height:16px}
.check{display:flex;gap:10px;align-items:center;color:var(--text);font-size:.88rem}
button.btn{background:var(--accent);color:var(--accent-ink);border:none;border-radius:4px;
font-weight:700;font-size:.82rem;padding:9px 16px;cursor:pointer}
button.btn:hover{filter:brightness(1.08)}
button.ghost{background:none;border:1px solid var(--line);color:var(--muted);border-radius:4px;
font-size:.8rem;padding:7px 12px;cursor:pointer}
button.ghost:hover{border-color:var(--accent);color:var(--accent)}
button.rm{background:none;border:1px solid rgba(255,90,90,.4);color:var(--bad);border-radius:4px;cursor:pointer;padding:6px 10px}
.savebar{background:var(--panel2);border-top:1px solid var(--line);padding:12px 16px;
display:flex;gap:12px;align-items:center;flex-wrap:wrap}
.savebar .mut{font-size:.78rem}
.noterow{padding:10px 16px;color:var(--muted);font-size:.82rem}
.tbar{display:flex;gap:10px;align-items:center;padding:10px 14px;border-bottom:1px solid var(--line);flex-wrap:wrap}
.tbar input{max-width:300px}.tbar select{width:auto}
.tbar .mut{font-size:.76rem;margin-left:auto}
th.sortcol{cursor:pointer}th.sortcol:hover{color:var(--accent)}
.saved{background:var(--good-bg);color:var(--good);font-weight:600;font-size:.85rem;
padding:9px 14px;border-radius:4px;margin-bottom:14px}
.err{background:var(--bad-bg);color:var(--bad);font-weight:600;font-size:.85rem;
padding:9px 14px;border-radius:4px;margin-bottom:14px}
footer{color:var(--faint);font-size:.76rem;text-align:center;margin-top:20px}
code{font-family:var(--mono);font-size:.82em;background:var(--panel2);padding:1px 5px;border-radius:3px}
a{color:var(--accent)}
.login{max-width:360px;margin:10vh auto 0}
"""

_SCRIPT = """<script>
function cellVal(tr,i){var td=tr.cells[i];if(!td)return'';var v=td.getAttribute('data-v');
if(v!==null){var n=parseFloat(v);return isNaN(n)?v:n;}return td.textContent.trim();}
function applyView(tb){
 var q=((document.getElementById('flt_'+tb.id)||{}).value||'').toLowerCase();
 var max=parseInt((document.getElementById('pgs_'+tb.id)||{}).value||'0')||0;
 var shown=0,total=0;
 tb.querySelectorAll('tbody tr').forEach(function(r){
  var ok=!q||r.textContent.toLowerCase().indexOf(q)>=0;
  if(ok)total++;
  var vis=ok&&(!max||shown<max);
  if(vis)shown++;
  r.style.display=vis?'':'none';
 });
 var n=document.getElementById('cnt_'+tb.id);if(n)n.textContent='showing '+shown+' of '+total;
}
document.querySelectorAll('table.sortable').forEach(function(tb){
 var dir={};
 tb.querySelectorAll('thead th').forEach(function(th,i){
  th.classList.add('sortcol');
  th.addEventListener('click',function(){
   dir[i]=!dir[i];
   var rows=Array.from(tb.querySelectorAll('tbody tr'));
   rows.sort(function(a,b){
    var x=cellVal(a,i),y=cellVal(b,i);
    if(typeof x=='number'&&typeof y=='number')return dir[i]?x-y:y-x;
    return dir[i]?String(x).localeCompare(String(y)):String(y).localeCompare(String(x));
   });
   var tbody=tb.querySelector('tbody');
   rows.forEach(function(r){tbody.appendChild(r)});
   applyView(tb);
  });
 });
 applyView(tb);
});
function editEntry(b){
 var d=b.closest('tr').dataset;
 document.getElementById('f_id').value=d.id;
 document.getElementById('f_domain').value=d.domain;
 document.getElementById('f_ticker').value=d.ticker;
 document.getElementById('f_name').value=d.name;
 document.getElementById('f_precision').value=d.precision;
 document.getElementById('f_verified').checked=(d.verified==='1');
 document.getElementById('editor').scrollIntoView({behavior:'smooth'});
 document.getElementById('f_domain').focus();
}
</script>"""


def _page(reg, title, body, chips=""):
    esc = html.escape
    return "".join([
        "<!doctype html><html lang=en><head><meta charset=utf-8>",
        '<meta name=viewport content="width=device-width, initial-scale=1">',
        "<title>", esc(title), "</title><style>", _CSS, "</style></head><body><div class=wrap>",
        "<header><div class=mark>{&#183;}</div><div class=htitle><h1>", esc(reg.name()), "</h1>",
        '<div class=sub>asset id &rarr; ticker, name, issuer, verified</div></div>',
        '<div class=hchips>', chips, "</div></header>",
        body,
        "<footer>", esc(reg.name()),
        " \xb7 the registry maps on-chain asset ids to human names and marks which issuers the registrar verified",
        "</footer></div>", _SCRIPT, "</body></html>",
    ])


def _fmt_age(ts):
    if not ts:
        return "never"
    d = int(time.time() - ts)
    if d < 90: return "%d s ago" % d
    if d < 5400: return "%d min ago" % (d // 60)
    if d < 172800: return "%d h ago" % (d // 3600)
    return "%d d ago" % (d // 86400)


def _assets_table(reg, admin, csrf_token=""):
    esc = html.escape
    merged = reg.merged()
    page = int(reg.ui().get("page_size", 50) or 50)
    opts = "".join('<option value="%d"%s>%d</option>' % (n, " selected" if n == page else "", n)
                   for n in (10, 25, 50, 100, 250))
    rows = []
    for aid in sorted(merged, key=lambda a: merged[a][1].upper()):
        domain, ticker, name, precision, verified = merged[aid]
        prov = reg.provenance(aid)
        prov_pill = {"upstream": '<span class="pill" style="background:var(--panel2);color:var(--muted)">upstream</span>',
                     "edited": '<span class="pill warn">edited here</span>',
                     "local": '<span class="pill good">local</span>'}[prov]
        actions = ""
        if admin:
            actions = "".join([
                "<td>",
                "<button type=button class=ghost onclick=editEntry(this)>Edit</button> ",
                '<form method=post action=/delete-entry style="display:inline">',
                '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
                '<input type=hidden name=asset_id value="', aid, '">',
                "<button class=rm title='Remove from this registry'>&times;</button></form>",
                ('<form method=post action=/restore-entry style="display:inline">'
                 '<input type=hidden name=csrf_token value="' + esc(csrf_token) + '">'
                 '<input type=hidden name=asset_id value="' + aid + '">'
                 "<button class=ghost title='Drop the local change; show the upstream value'>Restore</button></form>"
                 if prov != "upstream" else ""),
                "</td>",
            ])
        rows.append("".join([
            '<tr data-id="', aid, '" data-domain="', esc(domain), '" data-ticker="', esc(ticker),
            '" data-name="', esc(name), '" data-precision="', str(precision), '" data-verified="', str(verified), '">',
            '<td data-v="', esc(ticker.upper()), '"><b>', esc(ticker), "</b></td>",
            "<td>", esc(name), "</td>",
            '<td class=mono title="', aid, '">', aid[:10], "&hellip;", aid[-6:], "</td>",
            "<td>", esc(domain), "</td>",
            '<td class="num" data-v="', str(precision), '">', str(precision), "</td>",
            '<td data-v="', str(1 - verified), '">',
            ('<span class="pill good">VERIFIED</span>' if verified else '<span class="pill warn">unverified</span>'),
            "</td>",
            "<td>", prov_pill, "</td>",
            actions, "</tr>",
        ]))
    if not rows:
        return '<div class=noterow>The registry is empty — sync the upstream or add an asset.</div>'
    return "".join([
        '<div class=tbar>',
        '<input id=flt_reg placeholder="filter: ticker, name, id, domain, verified…" oninput=applyView(reg)>',
        '<select id=pgs_reg onchange=applyView(reg)>', opts,
        '<option value=0', " selected" if not page else "", ">all</option></select>",
        '<span id=cnt_reg class=mut></span></div>',
        '<table class=sortable id=reg><thead><tr><th>Ticker</th><th>Name</th><th>Asset id</th>',
        "<th>Issuer domain</th><th>Precision</th><th>Verified</th><th>Source</th>",
        ("<th></th>" if admin else ""), "</tr></thead><tbody>",
        "".join(rows), "</tbody></table>",
    ])


def _kpis(reg):
    merged = reg.merged()
    verified = sum(1 for e in merged.values() if e[4])
    local = sum(1 for a in merged if reg.provenance(a) != "upstream")
    if reg.upstream_ok is True:
        up = '<span class="pill good">reachable</span>'
    elif reg.upstream_ok is False:
        up = '<span class="pill bad">down — serving last good copy</span>'
    else:
        up = '<span class="pill warn">not configured</span>'
    return "".join([
        "<div class=kpis>",
        '<div class=kpi><div class=l>Assets</div><div class=v>', str(len(merged)),
        "</div><div class=s>", str(verified), " verified</div></div>",
        '<div class=kpi><div class=l>Local entries</div><div class=v>', str(local),
        "</div><div class=s>added or edited on this registry</div></div>",
        '<div class=kpi><div class=l>Upstream</div><div class=v style="font-size:.85rem">', up,
        "</div><div class=s>last sync ", _fmt_age(reg.upstream_ts), "</div></div>",
        '<div class=kpi><div class=l>Serving</div><div class=v style="font-size:.85rem">index.minimal.json</div>',
        '<div class=s>drop-in registry URL</div></div>',
        "</div>",
    ])


def _render_public(reg):
    body = "".join([
        _kpis(reg),
        "<div class=card><h2>Assets</h2>", _assets_table(reg, admin=False), "</div>",
        "<div class=card><h2>Use this registry</h2><div class=noterow>",
        "Machine-readable index: <code>GET /registry/index.minimal.json</code> — the format every Sequentia tool reads: ",
        "<code>{&quot;&lt;asset-id&gt;&quot;: [domain, ticker, name, precision, verified]}</code>. ",
        "Point a node at it with <code>assetregistryurl=&lt;this URL&gt;</code> in elements.conf, ",
        "or a price server via its <i>Asset registry URL</i> field.",
        "</div></div>",
    ])
    chips = '<span class="chip live">Live</span>'
    return _page(reg, reg.name(), body, chips)


def _render_login(reg, msg=""):
    err = '<div class=err>' + html.escape(msg) + "</div>" if msg else ""
    body = "".join([
        '<div class=login><div class=card><h2>Admin login</h2><div class=frm>', err,
        '<form method=post action=/login>',
        '<input type=password name=password placeholder="admin password" autofocus>',
        '<div style="margin-top:10px"><button class=btn type=submit>Log in</button></div>',
        "</form></div></div></div>",
    ])
    return _page(reg, reg.name() + " — login", body)


def _render_admin(reg, csrf_token, saved=False, error=""):
    esc = html.escape
    ui = reg.ui()
    ck = lambda v: "checked" if v else ""
    has_pw = bool(ui.get("password_hash"))
    banner = ('<div class=saved>Saved.</div>' if saved else "") + \
             ('<div class=err>' + esc(error) + "</div>" if error else "")
    body = "".join([
        banner,
        _kpis(reg),
        "<div class=card><h2>Assets <span class=right>local changes override the upstream and survive syncs</span></h2>",
        _assets_table(reg, admin=True, csrf_token=csrf_token), "</div>",

        # editor: add a new asset or (via the Edit buttons) change an existing one
        '<div class=card id=editor><h2>Add or edit an asset</h2>',
        '<form method=post action=/save-entry><div class=frm>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
        '<div class=frow><label>Asset id (64 hex)</label><input class=mono name=asset_id id=f_id ',
        'placeholder="the on-chain asset id" pattern="[0-9a-fA-F]{64}" required></div>',
        '<div class=frow><label>Ticker</label><input name=ticker id=f_ticker placeholder="GOLD" required style="max-width:220px"></div>',
        '<div class=frow><label>Name</label><input name=name id=f_name placeholder="Gold (troy ounce)" required></div>',
        '<div class=frow><label>Issuer domain</label><input name=domain id=f_domain placeholder="issuer.example"></div>',
        '<div class=frow><label>Precision (decimals)</label><input class=num name=precision id=f_precision value="8" style="max-width:120px"></div>',
        '<label class=check><input type=checkbox name=verified id=f_verified> Verified ',
        '<span class=mut>your statement, as the registrar, that you checked this issuer — wallets and nodes only trust verified entries for display</span></label>',
        '</div><div class=savebar><button class=btn type=submit>Save asset</button>',
        '<span class=mut>an existing id is overwritten; a new id is added as a local entry</span></div></form></div>',

        # upstream & sync
        '<div class=card><h2>Upstream &amp; sync</h2>',
        '<form method=post action=/settings><div class=frm>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
        '<div class=frow><label>Registry name</label><input name=registry_name value="', esc(reg.name()), '"></div>',
        '<div class=frow><label>Upstream registry URL</label><div><input class=mono name=upstream_url value="',
        esc(str(reg.cfg.get("upstream_url", DEFAULT_UPSTREAM))),
        '"><div class=hint>Synced periodically; the last good copy is kept and served when the upstream is down. ',
        'Leave empty to run standalone (no mirror, only local entries).</div></div></div>',
        '<div class=frow><label>Sync interval (seconds)</label><input class=num name=sync_interval value="',
        esc(str(reg.cfg.get("sync_interval_secs", 300))), '" style="max-width:120px"></div>',
        '</div><div class=savebar><button class=btn type=submit>Save settings</button>',
        '<button class=btn formaction=/sync-now>Sync now</button>',
        "<span class=mut>upstream ", ("reachable" if reg.upstream_ok else "DOWN" if reg.upstream_ok is False else "never synced"),
        " \xb7 last sync ", _fmt_age(reg.upstream_ts), "</span></div></form></div>",

        # access
        '<div class=card><h2>Access</h2>',
        '<form method=post action=/access><div class=frm>',
        '<input type=hidden name=csrf_token value="', esc(csrf_token), '">',
        '<label class=check><input type=checkbox name=public_page ', ck(ui.get("public_page", True)),
        "> Public registry page <span class=mut>the human-readable table at /</span></label>",
        '<label class=check><input type=checkbox name=public_index ', ck(ui.get("public_index", True)),
        "> Public JSON index <span class=mut><code>/registry/index.minimal.json</code> — turning this off breaks every consumer pointed here</span></label>",
        '<div class=frow><label>Rate limit (requests / min / IP)</label><input class=num name=api_rate_limit value="',
        esc(str(ui.get("api_rate_limit_per_min", 60))), '" style="max-width:120px"></div>',
        '<div class=frow><label>Table rows per page (default)</label><input class=num name=page_size value="',
        esc(str(ui.get("page_size", 50))), '" style="max-width:120px"></div>',
        ("<div class=frow><label>Current password</label><input type=password name=cur_password autocomplete=off></div>" if has_pw else
         "<div class=hint>No password set: the admin area only answers to this machine (127.0.0.1).</div>"),
        '<div class=frow><label>New password</label><input type=password name=new_password autocomplete=new-password></div>',
        '<div class=frow><label>Repeat new password</label><input type=password name=new_password2 autocomplete=new-password></div>',
        '</div><div class=savebar><button class=btn type=submit>Save access settings</button>',
        '<a class=mut href="/admin/export.json">Download registry + overrides</a></div></form></div>',
    ])
    chips = ('<span class="chip live">Running</span><span class=chip><a href="/">public view</a></span>'
             + ('<span class=chip><a href="/logout">log out</a></span>' if has_pw else ""))
    return _page(reg, reg.name() + " — admin", body, chips)


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------

def _is_loopback(host):
    if host in ("127.0.0.1", "::1", "localhost"):
        return True
    try:
        import ipaddress
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def serve(reg, host, port):
    csrf_token = secrets.token_urlsafe(32)
    sessions = {}
    SESSION_TTL = 12 * 3600
    limiter = _RateLimiter()

    def _same_origin(handler):
        host_hdr = handler.headers.get("Host", "")
        src = handler.headers.get("Origin") or handler.headers.get("Referer")
        if src is None:
            return False
        try:
            return urllib.parse.urlsplit(src).netloc == host_hdr
        except Exception:
            return False

    class Handler(http.server.BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="text/html; charset=utf-8", headers=None):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def _redirect(self, where, headers=None):
            self.send_response(303)
            self.send_header("Location", where)
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()

        def _client_local(self):
            return self.client_address[0] in ("127.0.0.1", "::1")

        def _authed(self):
            pw = reg.ui().get("password_hash")
            if not pw:
                return self._client_local()
            m = re.search(r"(?:^|;\s*)rsid=([A-Za-z0-9_-]+)", self.headers.get("Cookie", ""))
            if not m:
                return False
            exp = sessions.get(m.group(1))
            if not exp or exp < time.time():
                sessions.pop(m.group(1), None)
                return False
            sessions[m.group(1)] = time.time() + SESSION_TTL
            return True

        def _public_gate(self, enabled):
            if self._authed():
                return None
            if not enabled:
                self._send(403, json.dumps({"error": "disabled by the operator"}), "application/json")
                return True
            per_min = int(reg.ui().get("api_rate_limit_per_min", 60) or 60)
            if not limiter.allow(self.client_address[0], per_min):
                self._send(429, json.dumps({"error": "rate limit exceeded, slow down"}), "application/json",
                           headers={"Retry-After": "30"})
                return True
            return None

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path in ("/registry/index.minimal.json", "/index.minimal.json"):
                if self._public_gate(reg.ui().get("public_index", True)): return
                self._send(200, json.dumps(reg.merged(), indent=2, sort_keys=True), "application/json")
            elif path == "/":
                if self._authed():
                    self._redirect("/admin")
                    return
                if self._public_gate(reg.ui().get("public_page", True)): return
                self._send(200, _render_public(reg))
            elif path == "/public":
                if self._public_gate(reg.ui().get("public_page", True)): return
                self._send(200, _render_public(reg))
            elif path == "/admin":
                if self._authed():
                    self._send(200, _render_admin(reg, csrf_token, saved=self.path.endswith("saved=1")))
                else:
                    self._send(200, _render_login(reg))
            elif path == "/admin/export.json":
                if not self._authed():
                    self._redirect("/admin"); return
                self._send(200, json.dumps({"merged": reg.merged(), "overrides": reg.overrides,
                                            "upstream_fetched_at": reg.upstream_ts}, indent=2),
                           "application/json",
                           headers={"Content-Disposition": "attachment; filename=asset-registry-export.json"})
            elif path == "/logout":
                m = re.search(r"(?:^|;\s*)rsid=([A-Za-z0-9_-]+)", self.headers.get("Cookie", ""))
                if m:
                    sessions.pop(m.group(1), None)
                self._redirect("/admin", headers={"Set-Cookie": "rsid=; Max-Age=0; Path=/"})
            else:
                self._send(404, "not found")

        def do_POST(self):
            path = urllib.parse.urlsplit(self.path).path
            n = int(self.headers.get("Content-Length", 0) or 0)
            if n > 1_000_000:
                self._send(413, "request too large"); return
            raw = self.rfile.read(n)

            # Registration API: public, unauthenticated, JSON. Anyone may submit
            # an asset; what decides the outcome is the chain and their domain,
            # not this operator's opinion, so there is nothing here to gate on.
            if path in ("/", "/registry/") and \
                    self.headers.get("Content-Type", "").split(";")[0].strip() == "application/json":
                if self._public_gate(reg.ui().get("public_registration", True)): return
                try:
                    body = json.loads(raw.decode() or "{}")
                    result = register_asset(reg, body.get("asset_id"), body.get("contract"))
                    self._send(200, json.dumps(result, indent=2), "application/json")
                except ApiError as e:
                    self._send(e.status, json.dumps({"error": e.message}, indent=2), "application/json")
                except json.JSONDecodeError:
                    self._send(400, json.dumps({"error": "body must be JSON"}), "application/json")
                except Exception as e:
                    log.exception("registration failed")
                    self._send(500, json.dumps({"error": str(e)}), "application/json")
                return

            form = urllib.parse.parse_qs(raw.decode())
            g = lambda k, d="": form.get(k, [d])[0]

            if path == "/login":
                pw_hash = reg.ui().get("password_hash")
                if not pw_hash:
                    self._redirect("/admin"); return
                time.sleep(0.3)
                if _check_password(pw_hash, g("password")):
                    tok = secrets.token_urlsafe(32)
                    sessions[tok] = time.time() + SESSION_TTL
                    self._redirect("/admin", headers={
                        "Set-Cookie": "rsid=%s; HttpOnly; SameSite=Strict; Path=/" % tok})
                else:
                    log.warning("admin login failed from %s", self.client_address[0])
                    self._send(200, _render_login(reg, "Wrong password."))
                return

            if not self._authed():
                self._send(403, "forbidden: not logged in"); return
            if not _same_origin(self):
                self._send(403, "forbidden: cross-origin request"); return
            if not hmac.compare_digest(g("csrf_token"), csrf_token):
                self._send(403, "forbidden: invalid CSRF token"); return

            try:
                if path == "/save-entry":
                    aid = g("asset_id").strip().lower()
                    if not HEX64.match(aid):
                        self._send(200, _render_admin(reg, csrf_token,
                                                      error="Asset id must be exactly 64 hex characters.")); return
                    ticker = g("ticker").strip()
                    name = g("name").strip()
                    if not ticker or not name:
                        self._send(200, _render_admin(reg, csrf_token,
                                                      error="Ticker and name are required.")); return
                    try:
                        precision = max(0, min(8, int(g("precision", "8"))))
                    except ValueError:
                        precision = 8
                    entry = [g("domain").strip(), ticker, name, precision, 1 if g("verified") else 0]
                    reg.set_entry(aid, entry)
                elif path == "/delete-entry":
                    reg.delete_entry(g("asset_id").strip().lower())
                elif path == "/restore-entry":
                    reg.restore_entry(g("asset_id").strip().lower())
                elif path == "/sync-now":
                    reg.sync()
                elif path == "/settings":
                    reg.cfg["registry_name"] = g("registry_name").strip() or DEFAULT_NAME
                    reg.cfg["upstream_url"] = g("upstream_url").strip()
                    try:
                        reg.cfg["sync_interval_secs"] = max(30, int(g("sync_interval", "300")))
                    except ValueError:
                        pass
                    reg.save_config()
                elif path == "/access":
                    ui = dict(reg.cfg.get("ui", {}))
                    ui["public_page"] = bool(g("public_page"))
                    ui["public_index"] = bool(g("public_index"))
                    try:
                        ui["api_rate_limit_per_min"] = max(1, int(g("api_rate_limit", "60")))
                    except ValueError:
                        pass
                    try:
                        ui["page_size"] = max(0, int(g("page_size", "50")))
                    except ValueError:
                        pass
                    new_pw = g("new_password")
                    if new_pw:
                        cur_hash = reg.ui().get("password_hash")
                        if cur_hash and not _check_password(cur_hash, g("cur_password")):
                            self._send(200, _render_admin(reg, csrf_token, error="Current password is wrong.")); return
                        if new_pw != g("new_password2"):
                            self._send(200, _render_admin(reg, csrf_token, error="The new passwords don't match.")); return
                        if len(new_pw) < 8:
                            self._send(200, _render_admin(reg, csrf_token,
                                                          error="Password too short — use at least 8 characters.")); return
                        ui["password_hash"] = _hash_password(new_pw)
                    reg.cfg["ui"] = ui
                    reg.save_config()
                else:
                    self._send(404, "not found"); return
            except Exception as e:
                self._send(400, "error: " + html.escape(str(e))); return
            self._redirect("/admin?saved=1")

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer((host, port), Handler)
    log.info("asset registry on http://%s:%d (admin at /admin)", host, port)
    httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", required=True, help="path to JSON config file")
    parser.add_argument("--port", type=int, default=8092, help="listen port (default 8092)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address (default: localhost only)")
    parser.add_argument("--allow-remote", action="store_true",
                        help="permit a non-loopback bind even without an admin password (NOT recommended)")
    parser.add_argument("--set-password", action="store_true",
                        help="prompt for an admin password, store its hash in the config, and exit")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")

    if os.path.exists(args.config):
        with open(args.config) as f:
            config = json.load(f)
    else:
        config = {"upstream_url": DEFAULT_UPSTREAM, "sync_interval_secs": 300,
                  "registry_name": DEFAULT_NAME, "ui": {}}

    if args.set_password:
        import getpass
        pw = getpass.getpass("New admin password: ")
        if len(pw) < 8:
            parser.error("password too short — use at least 8 characters")
        if pw != getpass.getpass("Repeat it: "):
            parser.error("the passwords don't match")
        config.setdefault("ui", {})["password_hash"] = _hash_password(pw)
        _atomic_write(args.config, config)
        print("Admin password set. Restart the registry server to pick it up.")
        return 0

    if (not _is_loopback(args.host)
            and not config.get("ui", {}).get("password_hash")
            and not args.allow_remote):
        parser.error(
            "refusing to bind to non-loopback address %r without an admin password: "
            "anyone could rewrite your registry. Set one first with --set-password. "
            "--allow-remote overrides, at your own risk." % args.host)

    reg = Registry(config, args.config)
    if not os.path.exists(args.config):
        reg.save_config()
    threading.Thread(target=reg.sync_loop, daemon=True).start()
    serve(reg, args.host, args.port)


if __name__ == "__main__":
    sys.exit(main())
