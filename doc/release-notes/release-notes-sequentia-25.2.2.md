# Sequentia Core 25.2.2

25.2.2 is 25.2.1 plus two fixes found in an independent review of the audit
fixes. It changes no consensus rule, and the fork at height **163,000** is the
same. **Run 25.2.2 before the testnet reaches 163,000**: 25.2.0 and 25.2.1
accept the same blocks, but carry the first defect below.

## Stake records in the registry

A node keeps a stake registry (who stakes, who delegates to whom, which payout
policy binds) that it updates as blocks connect, and rebuilds from the UTXO set
when it starts. It applied a block's delegation and payout records all at
once: every record created, then every record spent. A record spent and
re-created with the same content in one block was erased right after being
re-added. The hardening fork refuses such a re-creation byte for byte, but a
record re-encoded with the same content still got through: a running node lost
it while a restarted node kept it, and the two computed different committees.

Records are now applied one transaction at a time, in block order, each
removing what it spends before adding what it creates: exactly how the UTXO set
changes, whatever the encoding. The registry a running node keeps always
matches the one it rebuilds at start-up.

## Pool claim fees

`claimpoolrewards`, and the GUI's "Collect my pool rewards", priced the network
fee in reference fee units and paid that number unconverted in the asset of the
pool's rewards. For any asset whose exchange rate is not one, the fee was too
small to relay or far too large. The fee is now converted, and paid in an asset
this node accepts for fees.

The GUI's confirmation when announcing a split payout policy no longer mentions
the 100-delegator limit, which the payout rounds of 25.2.0 removed.

## Upgrading

Stop the node, replace `sequentiad`, `sequentia-cli` and `sequentia-qt`, start
it. Check that `sequentiad -version` reports v25.2.2.
