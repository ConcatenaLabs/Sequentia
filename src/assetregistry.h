// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETREGISTRY_H
#define BITCOIN_ASSETREGISTRY_H

class CScheduler;

/**
 * SEQUENTIA Asset Registry client.
 *
 * When -assetregistryurl is set, the node fetches the registry's minimal index
 * (id -> [domain, ticker, name, precision]) and merges the tickers into the
 * global asset directory, so RPC output and the node GUI (which both read
 * gAssetsDir) show verified, centrally-maintained labels with no manual
 * -assetdir config. The merge is additive and never overrides an operator
 * -assetdir entry or the native asset label.
 */

/** Fetch the registry index once and merge it into the global asset dir.
 *  @return the number of newly-added labels, 0 if no URL is configured, or -1 on
 *  a fetch/parse error (which is logged, not thrown). */
int RefreshAssetRegistry();

/** Schedule an initial fetch shortly after startup plus periodic refreshes every
 *  -assetregistrypoll seconds. No-op if -assetregistryurl is unset. */
void StartAssetRegistry(CScheduler& scheduler);

#endif // BITCOIN_ASSETREGISTRY_H
