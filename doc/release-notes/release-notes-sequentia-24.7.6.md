# Sequentia Core 24.7.6

The checkpoint scan fetches parent-chain blocks raw and takes them apart itself,
rather than asking the parent daemon to decode each one into JSON. The first
anchor tick after a start, on a 100,000-block chain, drops from **70 seconds to
38**.

There is no consensus change here, and no change to which blocks are scanned or
what counts as a checkpoint.

## What was slow

The scan looks for one thing: `OP_RETURN` outputs carrying a checkpoint. It was
asking for each block at verbosity 2, which makes the parent daemon decode the
whole thing — every input, every witness, every address — so that this could
read the output scripts back out of the result. Measured on testnet4 the JSON
runs **over three times the size of the block itself**, and the daemon does that
work per block, on a scan that pulls a hundred of them the first time it runs.

Fetched raw, the same block is parsed here with the Bitcoin block type the node
already has for peg-ins. The daemon does almost nothing, and a third as much
crosses the wire.

## The block's own hash is what makes it safe

Parsing moved out of a daemon that has done it correctly for fifteen years and
into this file, so the parse is checked rather than trusted: a block that
deserializes **and hashes to the hash we asked for** was parsed correctly. One
that does not — however plausibly it parsed — is discarded. Nothing is recorded
until after that check, so a wrong parse cannot quietly become a wrong
checkpoint.

Both paths now record checkpoints through one function that takes an output
script, so they cannot come to different answers about the same output.

## Not every parent is bitcoind

The first version of this broke `feature_pos_parent_reorg_recovery`, which
drives an **Elements-mode** node as the parent — whose raw blocks are not
Bitcoin-serialized and do not deserialize here at all.

That constraint had been written down. The comment above the old fetch said it
worked "regardless of its transaction serialization", and it was removed as
stale while making this change. It was not stale; it was the reason the JSON
path existed.

So the JSON path is kept, as the general one. The raw path is tried first, and
one failed parse is enough to know what the daemon on the other end is: it says
so once and uses JSON from then on. Against a real Bitcoin parent the fallback
never fires.

## Testing

A new `checkpoint_scan_tests` starts from the question the change actually
raises — does taking a block apart here see exactly the outputs the daemon
reported? It embeds a real testnet4 block, eight transactions and sixteen
outputs including segwit and taproot, and requires the parsed output scripts to
equal, in order, what `getblock <hash> 2` returned for it. The expected scripts
come from the daemon, not from the code under test. Two further cases cover a
checkpoint being found and near-misses being left alone.
