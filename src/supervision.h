// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SUPERVISION_H
#define BITCOIN_SUPERVISION_H

#include <asset.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

class COutPoint;
class CTransaction;
class CTxOut;

/** SEQUENTIA: supervised assets.
 *
 *  A supervised asset is one whose issuer can freeze holders by consensus
 *  rule. The capability is declared at issuance and can never be added to an
 *  asset that was not born with it, removed, or altered, because the asset id
 *  is derived over the declaration: changing any field yields a different
 *  asset.
 *
 *  Design of record: doc/sequentia/supervised-assets-implementation.md, whose
 *  open questions were settled in
 *  doc/sequentia/alberto-supervised-assets-decisions-2026-08-13.md. The parts
 *  that constrain this file:
 *
 *  - A DESCRIPTOR is committed, not a bare key. An asset id is forever, so a
 *    field left out now can never reach an already-issued asset, and a later
 *    supervision variant would otherwise need its own derivation constant and
 *    become a separate asset class that every wallet and record parser written
 *    before it is blind to. Version and feature bits therefore exist from the
 *    first release even though nothing reads the bits yet.
 *
 *  - TWO keys. The operational key signs freezes and unfreezes; the recovery
 *    key, kept deep cold, can do exactly one thing, which is sign a rotation
 *    record replacing the operational key or itself. One key would mean any
 *    compromise forces migration of a live asset, and self-signed rotation is
 *    worse than none: attacker and issuer both hold the key, whoever rotates
 *    first wins permanently, so the attacker's opening move is to seize the
 *    authority silently.
 *
 *  - Keys are BIP340 x-only. Schnorr gives an issuer threshold signing for
 *    free, since FROST and MuSig2 both produce one ordinary signature under
 *    one ordinary key, and the protocol never learns a quorum is behind it.
 */

/** Descriptor version. Bump only for a change no feature bit can express. */
static const uint8_t SUPERVISION_VERSION = 1;

/** Feature bits.
 *
 *  RESERVED ONLY. None of these is implemented, and issuing an asset that sets
 *  one is refused (see ValidateSupervisionDescriptor) precisely so that the
 *  meaning stays free until somebody builds it. They exist because reserving a
 *  bit costs nothing now, whereas adding one later costs an asset-class
 *  migration for every asset already issued.
 */
enum SupervisionFeature : uint16_t {
    //! All scripts freezable, not only single-owner ones. The
    //! trapped-third-party problem that the single-owner rule exists to
    //! prevent would have to be handled a level up, by issuer reissuance or by
    //! delayed effectiveness on shared scripts.
    SUPERVISION_FEATURE_TOTAL     = (1 << 0),
    //! Asset-wide pause, expressed as a freeze record with a wildcard target.
    SUPERVISION_FEATURE_PAUSE     = (1 << 1),
    //! Securities-style transfer allowlists. Explicitly out of scope.
    SUPERVISION_FEATURE_WHITELIST = (1 << 2),
};

/** Every bit currently defined; anything outside this mask is malformed. */
static const uint16_t SUPERVISION_FEATURE_MASK =
    SUPERVISION_FEATURE_TOTAL | SUPERVISION_FEATURE_PAUSE | SUPERVISION_FEATURE_WHITELIST;

/** Serialize an x-only key as its bare 32 bytes.
 *
 *  XOnlyPubKey carries no serializer of its own, and a length prefix would be
 *  wasted on a fixed-width field whose bytes the asset id commits to. */
struct XOnlyPubKeyFormatter
{
    template <typename Stream>
    void Ser(Stream& s, const XOnlyPubKey& key) const
    {
        s.write(MakeByteSpan(key));
    }
    template <typename Stream>
    void Unser(Stream& s, XOnlyPubKey& key) const
    {
        unsigned char buf[32];
        s.read(MakeWritableByteSpan(buf));
        key = XOnlyPubKey(buf);
    }
};

/** The declaration committed into a supervised asset's id. */
struct SupervisionDescriptor
{
    uint8_t version{SUPERVISION_VERSION};
    uint16_t feature_bits{0};
    //! Signs freeze and unfreeze records. Rotatable via the recovery key.
    XOnlyPubKey operational_key;
    //! Signs rotation records only. Cannot freeze.
    XOnlyPubKey recovery_key;

    SERIALIZE_METHODS(SupervisionDescriptor, obj)
    {
        READWRITE(obj.version, obj.feature_bits,
                  Using<XOnlyPubKeyFormatter>(obj.operational_key),
                  Using<XOnlyPubKeyFormatter>(obj.recovery_key));
    }

    bool operator==(const SupervisionDescriptor& other) const
    {
        return version == other.version && feature_bits == other.feature_bits &&
               operational_key == other.operational_key && recovery_key == other.recovery_key;
    }
};

/** The serialized descriptor's fixed width: version || bits || two keys. */
static const size_t SUPERVISION_DESCRIPTOR_SIZE = 1 + 2 + 32 + 32;

/** A supervised asset and the terms it was born under, as carried on chain. */
struct SupervisionDeclaration
{
    //! The asset the descriptor governs. Redundant with the descriptor, in that
    //! the id is derived over it, but not recoverable from it: the derivation
    //! is one-way and also consumes the issuance prevout and contract hash.
    //! Naming the asset explicitly is what lets a node read the supervised-asset
    //! set straight out of the UTXO set. Issuance validation checks the two
    //! agree, so the redundancy cannot be used to lie.
    CAsset asset;
    SupervisionDescriptor descriptor;

    bool operator==(const SupervisionDeclaration& other) const
    {
        return asset == other.asset && descriptor == other.descriptor;
    }
};

/** Marker prefix identifying a supervision declaration output. */
extern const std::vector<unsigned char> SUPERVISION_MARKER;

/** Reject a descriptor that could not have been produced honestly.
 *
 *  Applied at issuance, so a malformed descriptor is refused rather than
 *  becoming a permanently broken asset. Both keys must be valid and distinct
 *  (equal keys would collapse the use/rotation separation that is the whole
 *  point of having two), and no unassigned or unimplemented feature bit may be
 *  set. */
bool ValidateSupervisionDescriptor(const SupervisionDescriptor& desc, std::string& err);

/** The descriptor's canonical hash, which is what the asset id commits to. */
uint256 SupervisionDescriptorHash(const SupervisionDescriptor& desc);

/** Build a declaration output's scriptPubKey.
 *
 *      <SUPERVISION_MARKER> OP_DROP <asset> OP_DROP <descriptor> OP_DROP OP_RETURN
 *
 *  Two things about the shape, both deliberate.
 *
 *  The declaration rides in an OUTPUT rather than in CAssetIssuance: extending
 *  that structure would change transaction serialization and break every parser
 *  in the ecosystem, while an output is inert to anything not looking for it.
 *  Note this is transport, not the binding. The binding is the asset id, which
 *  is derived over the descriptor, so a declaration that misstates its terms
 *  names an asset the issuance did not create, and issuance validation rejects
 *  it. Anyone can re-derive and check.
 *
 *  The OP_RETURN is at the END, not the start. A leading OP_RETURN would make
 *  the output provably unspendable, and AddCoin drops those on sight, which is
 *  exactly wrong here: the supervised-asset set has to be recoverable from the
 *  UTXO set so it can be rebuilt at startup and inverted on reorg the way the
 *  stake registry is (src/pos.h). Trailing, the script is still unspendable in
 *  every execution, but the coin is retained. It is one dust-free entry per
 *  supervised asset, permanently, which is the price of not introducing a
 *  second persistent index with its own reindex path to get wrong. */
CScript BuildSupervisionScript(const CAsset& asset, const SupervisionDescriptor& desc);

/** Parse a declaration output, or nullopt if it is not one. */
std::optional<SupervisionDeclaration> ParseSupervisionScript(const CScript& script);

/** Find the declaration carried by a transaction, if any.
 *
 *  Returns nullopt when the transaction declares nothing, which is the
 *  ordinary case. Sets `malformed` when it carries more than one declaration:
 *  one declaration governs one issuance, so a second has nothing to govern and
 *  is more likely a mistake than an intent worth guessing at. */
std::optional<SupervisionDeclaration> SupervisionFromTx(const CTransaction& tx, bool& malformed);

/** Entropy for a supervised issuance.
 *
 *  E = FastMerkleRoot( H(prevout), contract_hash, H(descriptor) )
 *
 *  The ordinary two-leaf form (GenerateAssetEntropy) is untouched, so an
 *  unsupervised issuance derives exactly as before and this whole feature is
 *  invisible to assets that do not use it. Because the asset id descends from
 *  the entropy, committing the descriptor here binds it to both the asset and
 *  its reissuance token with no further work.
 *
 *  Note what this implies for deployment, since it is the reason the rule needs
 *  an activation height rather than merely benefiting from one: consensus
 *  DERIVES an asset id rather than reading it from the transaction, so a node
 *  without this code derives a different id for the same issuance. A supervised
 *  asset therefore cannot exist before every node runs the new rules. */
void GenerateSupervisedAssetEntropy(uint256& entropy, const COutPoint& prevout,
                                    const uint256& contracthash,
                                    const SupervisionDescriptor& desc);

/** Validate a transaction that carries a supervision declaration.
 *
 *  Everything here is checked at issuance because none of it can be repaired
 *  afterwards: the descriptor is in the asset id, so an asset issued on wrong
 *  terms can only be abandoned. On success `entropy` receives the issuance
 *  entropy, which is what a reissuance will later have to quote.
 *
 *  The rules, and why each exists:
 *
 *  - Exactly ONE new issuance. One declaration governs one asset; with two
 *    issuances there is no honest answer to which the declaration describes.
 *  - The declared asset must equal the one the issuance actually derives. This
 *    is what makes the declaration unable to lie about its own terms.
 *  - Issuance amount and reissuance-token amount EXPLICIT, and the token amount
 *    non-zero. Alberto's point 3: seize and burn must never become consensus
 *    machinery, so they fall out economically instead, as freeze-as-burn and
 *    freeze-plus-reissue-as-seize. Reissuability is the one enabling condition,
 *    so an asset that cannot be reissued must not be able to call itself
 *    supervised.
 *  - EVERY output explicit in both asset and value. Consensus cannot read a
 *    blinded output's asset, so a supervised asset in one is unfreezable and
 *    unauditable. Blocking it at output creation and inducting from there is
 *    the only order that works: enforcing at spend time instead would strand
 *    whoever already held a blinded supervised output, permanently. It has to
 *    be every output, not merely the supervised ones, because which asset a
 *    blinded output carries is precisely what cannot be determined.
 *  - The declaration output itself carries zero of the declared asset, so an
 *    issuer cannot burn real value into a script nothing can ever spend. */
bool CheckSupervisedIssuance(const CTransaction& tx, const SupervisionDeclaration& decl,
                             uint256& entropy, std::string& err);

/** Height from which supervised assets exist on this chain; 0 means never.
 *
 *  A mirror of Consensus::Params::supervised_assets_height, set per chain in
 *  chainparams.cpp, following the g_con_elementsmode pattern
 *  (src/primitives/transaction.h). It exists because Consensus::CheckTxInputs
 *  is reached without Consensus::Params and is the one place both mempool
 *  acceptance and ConnectBlock pass through, which is what keeps them from
 *  disagreeing.
 *
 *  Convention, matching pos_exprace_height: 0 DISABLES, a positive H enforces
 *  from height H. A chain launched with the rule sets 1, never 0. */
extern int g_supervision_height;

/** Whether a transaction confirming at `height` derives supervised asset ids.
 *
 *  Consensus derives asset ids rather than reading them, so this gate is not a
 *  courtesy: below it a declaration is inert data and the issuance derives
 *  plainly, exactly as a node without this code would. Both sides of the
 *  boundary agree because the answer depends only on the height of the block
 *  the transaction confirms in. */
inline bool SupervisionActive(int height)
{
    return g_supervision_height > 0 && height >= g_supervision_height;
}

#endif // BITCOIN_SUPERVISION_H
