# SeqLN Tier 2: hosted channels via a CLN native signer split (design + milestones)

Status: design, 2026-07-04. Grounds the build of the Tier-2 backend the UX audit (§8, decision 8.9)
picks: **truly-instant pure-LN from a thin wallet, non-custodially** — we host the SeqLN node, the user's
device holds the keys and co-signs every channel state, so we can never move the user's funds. This is the
foundation the wallet Lightning UX hangs off; it is built before the UX overhaul.

Decision locked with the user (2026-07-04): the signer mechanism is the **CLN native signer split** (not VLS,
Greenlight, or a custodial interim). Rationale: smallest delta from our SeqLN fork, asset-native for free.

## 1. Why this is tractable (the three de-risking findings)

From a full file-anchored map of `~/seqln`'s signer (`hsmd/hsmd_wire.csv`, `hsmd/hsmd.c`, `hsmd/libhsmd.c`,
`lightningd/hsm_control.c`, `lightningd/lightningd.c:433`, `lightningd/options.c:365`):

1. **The seam is production, not a hack.** `--subdaemon=hsmd:PATH` (`options.c:1633-1642`, resolved
   `lightningd.c:433-451`) makes lightningd exec ANY binary in place of `lightning_hsmd`, provided it speaks
   the hsmd wire on fd 3. This is exactly how upstream deploys VLS
   (`--subdaemon=hsmd:/var/lib/vls/bin/remote_hsmd_socket`). No VLS/remote-signer code is in the fork — only
   the option + the example — so we build the signer, not modify lightningd.

2. **The signer is asset-agnostic.** `libhsmd.c` has ZERO `asset/elements/confidential/blind/nDenomination`
   references. Every `sign_*_tx` ends at `sign_tx_input`. The Elements sighash
   (`bitcoin/signature.c:120-151 bitcoin_tx_hash_for_sig`) feeds the input's EXPLICIT value
   (`psbt_input_get_amount` `bitcoin/psbt.c:645` returns the raw explicit sats "regardless of asset") and the
   already-serialized tx bytes; the asset tag is never a separate signer input. Transparent-by-default
   (principle 6) is exactly why: channel/HTLC values are explicit, so the sighash is deterministic with no
   blinding factors. A remote signer will not break on asset channels.

3. **The device crypto kernel is small + isolable.** `libhsmd` is already a dependency-free library. A device
   signer must cover only: BIP-39 (the fork's `hsm_secret` IS a mnemonic, `hsmd.c:307-357`), BIP-32 + BIP-86,
   HKDF-SHA256 derivations (node key, per-channel seed, shaseed), basepoint/per-commitment derivation, secp256k1
   ECDH, ECDSA low-R sign (+ BIP-340 if bolt12), and the Elements BIP-143 sighash (~40 lines over the standard
   preimage). All exist in Rust (`ambra_core` secp256k1 + rust-elements) and compile to WASM. No CLN event
   loop, DB, or gossip logic device-side.

## 2. Architecture

```
   DEVICE (browser WASM / phone Rust)                HOSTED (our box)
   +-------------------------------+                 +--------------------------------------+
   | device signer                 |  framed hsmd    | hsmd-proxy  (the `hsmd` binary)       |
   | - holds the mnemonic          |<== wire msgs ==>| - speaks hsmd wire on fd 3            |
   | - libhsmd crypto subset       |  {client_ctx,   | - multiplexes N per-subdaemon fds    |
   | - per-wallet policy/validation |   hsmd_msg}     |   LOCALLY (hsmd_client_hsmfd)         |
   +-------------------------------+   over 1 conn   | - tunnels each client's msgs to device|
                                                     +------------------|-------------------+
                                                                        | (local UNIX fds)
                                                     +------------------v-------------------+
                                                     | lightningd + channeld + gossipd +    |
                                                     | connectd + onchaind  (SeqLN fork)    |
                                                     | NO hsm_secret on disk                |
                                                     +--------------------------------------+
```

Load-bearing structural constraint (from the map): **fd multiplexing must stay in the LOCAL proxy.** The
`hsmd_client_hsmfd` (msg 9) → `io_send_fd` → `fdpass_recv` dance (`hsmd.c:601`, `hsm_control.c:20`) is
inherently local-UNIX (SCM_RIGHTS). So the `hsmd-proxy` binary terminates fd 3 from lightningd, answers
`hsmd_client_hsmfd` locally (socketpair + fd passback), and for every client fd forwards framed
`{client_ctx: node_id/dbid/capabilities, hsmd_wire_msg}` to the device over ONE transport. The device is a
pure request/response signer and never sees fd passing. Do NOT try to network-transport the fd handout.

The whole hsmd protocol is synchronous request/response (only status msgs are async, `hsm_control.c:88`), so a
network round-trip per sign is structurally fine — except ECDH (below).

Hosted lightningd runs with NO `hsm_secret` on disk: the device holds the mnemonic, the proxy originates
`hsmd_init` + `derive_secret` answers from the device at boot. Channels are UNANNOUNCED (private mobile
client), so the signer advertises NOT-capable for all gossip sigs and lightningd adapts (`hsm_control.c:180`).

## 3. Milestone plan

Incremental, each independently verifiable on the laptop harness (the M4/M5 SeqLN regtest + testnet4 nodes),
mirroring the pure-LN M0-M5 discipline.

- **M0 — prove the seam. DONE (2026-07-04).** Ran a full SeqLN node with `--subdaemon=hsmd:<our binary>`: the
  node booted end-to-end (`Server started with public key ...`), lightningd resolved + exec'd our substituted
  binary (`lightningd: testing .../hsmd-passthrough.sh`), the hsmd handshake completed (`mnemonic HSM secret`,
  capabilities advertised), and the hsmd wire protocol flowed through our binary (`Received message 27`
  derive_secret, `new_client: 0` fd handout). Confirms the production `--subdaemon` seam works on our fork with
  no lightningd change. Combined with the code-proof that the signer is asset-agnostic (§1.2), asset-channel +
  swap signing through a substituted signer is established; the runtime channel-signing trace lands with M1's
  logging proxy. NEXT: M1.
- **M1 — network split (out-of-process signer). DONE (2026-07-04, seqln `1e131300a`).** `hsmd-proxy`
  (`hsmd/hsmd_proxy.c`, substituted via `--subdaemon=hsmd:PATH`) keeps all ccan/io + fd machinery, answers
  `WIRE_HSMD_CLIENT_HSMFD` LOCALLY (fd-multiplexing never crosses the wire), loads no secret, and forwards every
  secret-bearing request over one socket (fork+socketpair) to `signerd` (`hsmd/signerd.c`), which owns the
  hsm_secret + libhsmd and reconstructs the `hsmd_client` per request. `hsmd/signer_frame.h` = the tiny
  little-endian frame (`is_main|node_id|dbid|capabilities|hsmd_msg`), self-describing for M2 reuse. VERIFIED:
  a node boots through the split AND a full channel lifecycle (connect ln2, fund, open→CHANNELD_NORMAL, mutual
  close) is signed out-of-process with valid Elements/asset signatures. Runtime channel-signing trace captured
  (ECDH, GET_CHANNEL_BASEPOINTS, NEW/SETUP_CHANNEL, GET_PER_COMMITMENT_POINT, SIGN_WITHDRAWAL,
  SIGN_REMOTE_COMMITMENT_TX, VALIDATE_COMMITMENT_TX, SIGN_MUTUAL_CLOSE_TX) — confirms the M2 subset.
- **M2 — device signer (Rust). DONE (2026-07-04, seqln `8664fdf01` M2a + `3a8a753ee` M2b).** Crate
  `contrib/seqln-signer`: an I/O-free (WASM-ready) crypto kernel + the hsmd message subset (§4), reimplementing
  libhsmd's derivations AND transaction signing in Rust. M2a: derivations byte-exact (35/35 synthetic). M2b:
  the Elements BIP-143 sighash + low-R grinding (the subtlety: libhsmd feeds 32 zero noncedata bytes on the
  first grind round; driven via `sign_ecdsa_with_noncedata`) + the `sign_*` handlers. Reply =
  64-byte-compact+sighash-byte. PROVEN: real-request corpus byte-exact vs libhsmd (fundee 99/0, funder 102/1 —
  the 1 = `sign_withdrawal`'s full-PSBT reply, out of the device-as-fundee role), AND a node running the Rust
  signer ALONE (no fallback) completed a full channel lifecycle (open→CHANNELD_NORMAL→120k-sat payment→mutual
  close), the peer validating every signature, 0 BROKEN. Crypto pinned to the ambra_core 0.32 line for phone +
  WASM. So the device signer is functionally complete for the pure-LN hosted-channel path.
- **NETWORK TRANSPORT + M3 — DONE (2026-07-04, seqln `0db3812bd`).** The signer split now runs over a real
  network link, making the hosted model concrete: `hsmd_proxy.c` `connect_remote_signer()` (env
  `SEQLN_SIGNER_ADDR`) reaches a REMOTE `seqln-signer --listen host:port` over TCP instead of fork+socketpair
  (fork stays the default). PROVEN: a hosted node with NO local `hsm_secret` derived its node_id from a
  separate, detached remote signer over TCP, then completed a full channel lifecycle (open→NORMAL→50k
  payment→mutual close) with every signature crossing the network. **M3 confirmed:** a GOLD asset channel to
  the hosted node reached CHANNELD_NORMAL and mutual-closed, the Rust signer signing the asset-channel
  commitments over the transport (Elements sighash serializes the GOLD asset/value commitment identically).
  Topology confirmed: the device node is the FUNDEE (inbound liquidity from the LP), so `sign_withdrawal` being
  out of the M2 subset is correct. **SECURITY (blocking for production, not for the demo):** the TCP link is
  unauthenticated + unencrypted; production MUST wrap it in a Noise/TLS + per-wallet-auth channel before the
  device signer holds real keys or faces a public network.
- **M4 — policy + ECDH latency.** `validate_commitment_tx` is a libhsmd stub (`:1898`); make the device a TRUE
  validating signer (verify amounts/destinations before signing — this is where "device co-signs" earns its
  keep vs a dumb signer). Resolve the ECDH hot-path strategy (device-authorized session key / delegated ECDH /
  batching) so peer-connect + onion-forward stay fast.
- **M5 — recovery / escape hatch.** Wire static-remotekey + SCB (`emergency.recover`) + peer-storage +
  `recover.c` so a user whose hosted node vanishes can force-close/sweep with only their device (mnemonic). The
  device already holds every secret to sign penalty + to-us sweeps.

Then Tier-2 hosted channels are available to the wallets: the wallet SDK opens a hosted channel (pay-to-open /
JIT liquidity from our LP), holds its keys, co-signs, and trades asset<->BTC over pure-LN truly instantly.

## 4. Device-signer minimal message subset

MUST implement (a hosted, private-channel, pure-LN wallet exercises only these):
- Boot/derivation: `init`(→ v4 reply, hsm_version 5-6, honest capabilities), `derive_secret`,
  `get_channel_basepoints`, `get_per_commitment_point`, `new_channel`/`setup_channel`/`forget_channel`,
  `check_outpoint`/`lock_outpoint`.
- Channel lifecycle: `sign_commitment_tx`, `sign_remote_commitment_tx`, `validate_commitment_tx`,
  `revoke_commitment_tx`, `validate_revocation`.
- HTLCs: `sign_remote_htlc_tx`, `sign_any_local_htlc_tx`.
- On-chain claim (onchaind): `sign_mutual_close_tx`, `sign_(any_)remote_htlc_to_us`,
  `sign_(any_)delayed_payment_to_us`, `sign_(any_)penalty_to_us`.
- Peer + payments: `ecdh` (hot path), `sign_invoice`, `sign_withdrawal`, `get_output_scriptpubkey`.
- Policy: `preapprove_invoice`/`keysend`(`_check`).

CAN skip (advertise NOT-capable so lightningd adapts): all gossip sigs (`node_announcement`,
`channel_announcement/update`) — channels are unannounced; `sign_splice_tx`, `sign_anchorspend`,
`sign_htlc_tx_mingle` unless splicing/anchors/BYO-fee enabled; `sign_bolt12*` unless offers;
`bip137_sign_message`; `check_pubkey`/`check_bip86_pubkey`.

## 5. Open decisions (resolve during M4)

- **ECDH latency (load-bearing).** ECDH (`common/ecdh_hsmd.c`) is on peer-connect + every onion hop; a naive
  per-ECDH network round-trip to a phone hurts connect + forward time. Options: a device-authorized ECDH
  session key provisioned at channel open; delegated node-ECDH via a subkey connectd can use; or batching.
  Decide with real M1 latency numbers. (Mirrors UX-spec §8.9.)
- **Validation policy depth (M4).** MVP can mirror the libhsmd stub (sign on request); the security win is a
  VLS-style policy (never sign a commitment that moves funds to a non-channel destination, enforce
  value-conservation, rate-limit). Depth vs latency tradeoff.
- **Backup transport.** SCB via peer-storage (the hosted node stashes the device's encrypted backup) vs a
  device-local export vs both. Recovery UX in the wallet.

## 6. Do not regress

The M0-M5 pure-LN + submarine flows (seqdex `phase3-pure-ln`), the asset-channel machinery (seqln
`825c5db`+`3ab36eec`+`0a21f40`), and the transparent-by-default explicit-value property that makes the signer
asset-agnostic. The hosted lightningd stays a normal SeqLN node; only the signer is externalized.
