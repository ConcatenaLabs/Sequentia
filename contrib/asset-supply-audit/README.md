# asset-supply-audit

Independently compute the exact circulating supply of a Sequentia asset from
chain data alone.

The node deliberately keeps no per-asset supply index: `coinstatsindex` sums
atoms across every asset into a single number and skips blinded outputs, and
`listissuances` only reports what one wallet knows. Supply is therefore not a
value you look up, it is a value you reconstruct:

    circulating = explicit issuances + explicit reissuances - explicit burns

This tool walks the chain over public RPC and does exactly that, trusting the
node for nothing but the block data it serves. Two parties running it against
their own nodes must agree to the atom, or one of their nodes is lying.

It exists because a bridged stablecoin's whole claim rests on provable backing:
see `doc/sequentia/bridged-usdc-standard.md`, which makes explicit issuance and
burn a normative requirement precisely so that this reconstruction is exact.

## Usage

    ./audit.py --asset <assetid> --datadir ~/.elements
    ./audit.py --asset <assetid> --rpc-url http://127.0.0.1:7041/ \
               --rpc-user U --rpc-password P --json

With no `--asset`, every asset seen on the chain is reported. `--checkpoint
FILE` persists progress and resumes, which matters on a long chain: the scan
reads every block, at roughly 400 blocks/second against a local node.

Amounts are reported in **atoms**, the integer consensus amount. An asset's
display precision is metadata the node never applies to amounts, so a
6-decimal asset showing 1,000,000 atoms holds 1.000000 units.

## Exactness is checked, not assumed

A blinded issuance, reissuance or burn hides its amount from every observer, so
an asset that has one does not have a knowable supply. When that happens the
tool labels the figure a bound, prints what is hidden, and exits with status 2.
Exit 0 means the number is exact.

This is the property a compliant bridged asset must maintain: holders may blind
their own transfers freely (that hides distribution, never supply), but the
issuer must keep every supply-changing event explicit.
