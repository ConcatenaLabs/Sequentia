# Vendored blst (BLS12-381)

This directory is a vendored, unmodified subset of the **blst** library by
Supranational, used for the autonomous Proof-of-Stake committee's BLS aggregate
signatures (`src/bls.{h,cpp}`; see
`doc/sequentia/proposals/autonomous-committee.md` §7).

- **Upstream:** https://github.com/supranational/blst
- **Commit:** `d29fef46c29a662ec215a9eeb9578e25f9146b6d`
- **Vendored subset:**
  - `bindings/` — `blst.h`, `blst_aux.h`, `blst.hpp` (the C and C++ APIs only)
  - `src/` — the C sources and the `server.c` amalgamation
  - `build/` — the `assembly.S` amalgamation and the per-platform `.s`/`.S` files
  - The Go/Rust/etc. language bindings and the upstream test suite are
    intentionally **not** vendored.
- **Local changes:** none to the C/assembly sources (byte-for-byte upstream).
  Only the build helper scripts (`refresh.sh`, `srcroot.go`, `bindings_trim.pl`)
  were removed.
- **Build:** an explicit rule in `../Makefile.am` compiles `libblst.a` from
  `src/server.c` + `build/assembly.S` (the x86-64 / ELF assembly path), with
  flags following blst's own recommended build.

Pre-mainnet, this vendored copy must be confirmed to match the named upstream
commit with no local patches — the same external-sign-off item recorded for the
secp256k1 MuSig2 module in `doc/sequentia/07-security-and-audit.md` §1.
