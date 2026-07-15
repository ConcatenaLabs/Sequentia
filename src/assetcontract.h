// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETCONTRACT_H
#define BITCOIN_ASSETCONTRACT_H

#include <uint256.h>

#include <string>
#include <vector>

class UniValue;

/**
 * An asset contract is the human-readable identity of an asset: its name,
 * ticker, precision and the internet domain of whoever issued it. The contract
 * is not stored on chain -- only SHA256 of its canonical serialisation is, as
 * the issuance input's contract_hash, which in turn feeds the asset id. That
 * commitment is what lets an asset registry prove, later, that a given piece of
 * metadata really belongs to a given asset id, and that the domain named in it
 * really authorised the link (by serving the proof file; see AssetProofLine).
 *
 * Because the hash is committed at issuance and the asset id is derived from
 * it, none of this can be added or corrected afterwards: an asset issued with a
 * zero contract_hash is unverifiable for as long as it exists. Issue it right or
 * not at all.
 *
 * The canonical form here is byte-for-byte the one the Sequentia asset registry
 * hashes (see the sequentia-registry repository, server.js: canonicalize()).
 * Diverging by a single byte yields a different hash, hence a different asset
 * id, and the registry then rejects the asset as not matching its on-chain
 * commitment. The unit tests pin this against a contract hashed by that
 * implementation.
 */
namespace AssetContract {

/**
 * Serialise `value` as canonical JSON: object keys sorted lexicographically by
 * their UTF-8 bytes, no insignificant whitespace, strings escaped as
 * JavaScript's JSON.stringify escapes them.
 */
std::string CanonicalJson(const UniValue& value);

/**
 * SHA256 of CanonicalJson(contract), as the issuance input wants it.
 *
 * The returned uint256 holds the digest in its natural byte order, which is the
 * order the issuance serialises and the order the asset-id derivation consumes.
 * Note this is NOT what parsing the digest's hex through the usual uint256
 * helpers gives you: those read hex big-endian and so reverse it. Passing a
 * SHA256 hex digest straight to issueasset's raw contract_hash argument commits
 * the reversed bytes and silently produces an asset no registry can verify.
 */
uint256 Hash(const UniValue& contract);

/**
 * Build a contract from its fields, with the keys the registry expects. The
 * result still needs Validate(); Build() only assembles.
 */
UniValue Build(const std::string& name,
               const std::string& ticker,
               int precision,
               const std::string& domain,
               const std::string& issuer_pubkey);

/**
 * Check `contract` against the rules the registry enforces on submission, so a
 * contract Core accepts is one the registry will too -- the alternative is
 * finding out after issuance, when nothing can be changed. Returns one message
 * per problem, empty if the contract is good.
 */
std::vector<std::string> Validate(const UniValue& contract);

/**
 * The exact one line the issuer must serve, as text/plain and with nothing else
 * in the body, to authorise linking `domain` to `asset_id`.
 */
std::string AssetProofLine(const std::string& domain, const std::string& asset_id);

/** Path component the registry fetches the proof from, relative to the domain root. */
std::string AssetProofPath(const std::string& asset_id);

/** Full https:// URL the registry fetches the proof from. */
std::string AssetProofUrl(const std::string& domain, const std::string& asset_id);

} // namespace AssetContract

#endif // BITCOIN_ASSETCONTRACT_H
