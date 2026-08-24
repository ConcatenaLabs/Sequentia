// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_PARENTCHAIN_H
#define BITCOIN_WALLET_PARENTCHAIN_H

#include <consensus/amount.h>
#include <key.h>
#include <primitives/bitcoin/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <uint256.h>

namespace wallet {
class CWallet;

/**
 * SEQUENTIA: signing on the PARENT chain.
 *
 * A Sequentia wallet's addresses are Bitcoin-identical, so the coins it
 * receives at them on the parent chain are spendable with its own keys. Two
 * things are needed to spend them and both live here rather than in one RPC's
 * private corner, because the cross-chain conversion path needs exactly the
 * same two -- and a second copy of a SIGHASH is the kind of thing that drifts,
 * after which nothing verifies anybody's signature.
 */

/** BIP143 (segwit v0) signature hash over the parent-chain transaction form.
 *
 *  `script_code` is the P2WPKH pseudo-script for a key spend, or the witness
 *  script itself for a P2WSH one -- which is how an HTLC claim on the parent
 *  chain is signed. */
uint256 ParentBip143Sighash(const Sidechain::Bitcoin::CMutableTransaction& tx, unsigned int in_pos,
                            const CScript& script_code, CAmount amount);

/** The wallet key behind a P2WPKH scriptPubKey, wherever the wallet keeps it. */
bool GetWalletKeyForP2WPKH(const CWallet& wallet, const CScript& spk, CKey& key, CPubKey& pubkey);

} // namespace wallet

#endif // BITCOIN_WALLET_PARENTCHAIN_H
