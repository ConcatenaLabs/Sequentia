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
 * (id -> [domain, ticker, name, precision, verified]) and merges the tickers of
 * VERIFIED entries into the global asset directory, so RPC output and the node
 * GUI (which both read gAssetsDir) show centrally-maintained labels with no
 * manual -assetdir config. The merge is additive and never overrides an operator
 * -assetdir entry or the native asset label.
 *
 * TRUST / SECURITY NOTE: these labels are ADVISORY. The registry index is
 * currently fetched over plain HTTP with no transport security and no signature
 * over the response, so a network man-in-the-middle (or a compromised registry)
 * can alter the labels and the node has no way to detect it. The "verified" flag
 * only reflects what the registry asserts (it performed chain + domain proof
 * checks); it is NOT a cryptographic guarantee to this client. Labels must never
 * be treated as authoritative for value/consensus decisions, only as a display
 * convenience. (Follow-up: serve the index over TLS and/or sign it, and verify
 * the signature here, before any label is shown without an "unverified" marker.)
 */

/** Fetch the registry index once and merge it into the global asset dir.
 *  @return the number of newly-added labels, 0 if no URL is configured, or -1 on
 *  a fetch/parse error (which is logged, not thrown). */
int RefreshAssetRegistry();

/** Schedule an initial fetch shortly after startup plus periodic refreshes every
 *  -assetregistrypoll seconds. No-op if -assetregistryurl is unset. */
void StartAssetRegistry(CScheduler& scheduler);

#endif // BITCOIN_ASSETREGISTRY_H
