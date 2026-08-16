// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SUPERVISION_H
#define BITCOIN_SUPERVISION_H

#include <asset.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

class CBlock;
class CBlockUndo;
class CCoinsView;

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
 *  One is implemented (PAUSE); the rest are reserved and refused at issuance,
 *  so their meaning stays free until somebody builds them. They exist because
 *  reserving a bit costs nothing now, whereas adding one later costs an
 *  asset-class migration for every asset already issued: the bits are in the
 *  asset id, so an asset cannot gain or lose one.
 */
enum SupervisionFeature : uint16_t {
    //! All scripts freezable, not only single-owner ones. RESERVED, not built.
    //! The trapped-third-party problem that the single-owner rule exists to
    //! prevent would have to be handled a level up, by issuer reissuance or by
    //! delayed effectiveness on shared scripts.
    SUPERVISION_FEATURE_TOTAL     = (1 << 0),
    //! IMPLEMENTED. Asset-wide pause: the issuer may freeze every single-owner
    //! holding of the asset at once, with one record naming the wildcard target
    //! (SUPERVISION_PAUSE_TARGET) instead of a script.
    //!
    //! Opt-in at issuance, and permanently so, because it is in the asset id.
    //! An issuer who never wants the power cannot be given it, and a holder can
    //! see at issuance whether the asset they are about to accept can be
    //! stopped wholesale. That visibility is the reason it is a committed bit
    //! rather than a rule every supervised asset gets.
    SUPERVISION_FEATURE_PAUSE     = (1 << 1),
    //! Securities-style transfer allowlists. RESERVED, explicitly out of scope.
    SUPERVISION_FEATURE_WHITELIST = (1 << 2),
};

/** Every bit currently defined; anything outside this mask is malformed. */
static const uint16_t SUPERVISION_FEATURE_MASK =
    SUPERVISION_FEATURE_TOTAL | SUPERVISION_FEATURE_PAUSE | SUPERVISION_FEATURE_WHITELIST;

/** The bits an asset may actually be issued with today.
 *
 *  Only PAUSE is implemented. The other two stay refused so their meaning
 *  remains free: an asset issued claiming a capability nothing enforces would
 *  be a permanent lie, since the bits are in the asset id and cannot be
 *  corrected afterwards. */
static const uint16_t SUPERVISION_FEATURE_IMPLEMENTED = SUPERVISION_FEATURE_PAUSE;

/** The wildcard freeze target: "every script holding this asset".
 *
 *  All-zero, which no real script hashes to, so it cannot collide with a
 *  targeted freeze. Pause is a freeze record naming this instead of a script
 *  (Alberto's point 7), not a new record type: it reuses signed admission, the
 *  registry, unfreeze-as-spend, reorg handling and mempool eviction unchanged,
 *  at the cost of one sentinel value. Lifting a pause is spending its record,
 *  exactly like any other freeze. */
static const uint256 SUPERVISION_PAUSE_TARGET = uint256();

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

/** The entropy of the initial issuance at input `vin` of `tx`, supervised or not.
 *
 *  For anything READING an issuance back out of a stored transaction, which is
 *  what a wallet does constantly: listing issuances, finding a reissuance token,
 *  recovering a contract, registering an asset's denomination. Deriving with
 *  GenerateAssetEntropy alone is right for an ordinary issuance and silently wrong
 *  for a supervised one -- it yields the id of an asset that does not exist, so
 *  the wallet reports an asset nobody holds and cannot find the reissuance token
 *  of the one it does. Consensus never has that problem because it derives from
 *  the declaration; this does the same.
 *
 *  The declaration output that carries the descriptor is in the issuance's own
 *  transaction (consensus requires it), and it names the asset it belongs to, so
 *  a supervised derivation is taken only when it reproduces that asset. A
 *  transaction carrying somebody else's declaration therefore cannot mislabel an
 *  ordinary issuance.
 *
 *  Caller must have checked that the issuance is an initial one (a null
 *  assetBlindingNonce); a reissuance quotes its entropy directly and has none to
 *  derive. */
void GenerateIssuanceEntropyFromTx(uint256& entropy, const CTransaction& tx, size_t vin);

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

/* -------------------------------------------------------------------------
 * Records: freezing, unfreezing and key rotation.
 * ---------------------------------------------------------------------- */

/** What a record does. */
enum class SupervisionRecordKind : uint8_t {
    //! Freeze one script for one asset. Creating the record freezes; spending
    //! it unfreezes.
    FREEZE = 1,
    //! Replace the operational key. Signed by the RECOVERY key.
    ROTATE_OPERATIONAL = 2,
    //! Replace the recovery key. Signed by the RECOVERY key.
    ROTATE_RECOVERY = 3,
};

/** An x-only key in the record's fixed-width payload slot.
 *
 *  A rotation reuses the freeze record's `target` field to carry the new key,
 *  so the two record shapes stay one parser and one width. Both are 32 bytes,
 *  which is the whole reason it works. */
inline uint256 SupervisionKeyAsTarget(const XOnlyPubKey& key)
{
    return uint256(key.data(), key.size());
}

/** A freeze or rotation record, as carried in an output.
 *
 *  `target` is the payload, read according to `kind`: for FREEZE it is the
 *  hash of the frozen scriptPubKey; for the rotations it is the new key. A
 *  rotation also carries `old_key`, the key it replaces, which is what lets the
 *  registry replay rotations from the UTXO set in any order: each record names
 *  its predecessor, so they chain from the descriptor rather than needing a
 *  timestamp the UTXO set does not keep.
 *
 *  `signature` is the ADMISSION signature, not a spend condition. See
 *  BuildSupervisionRecordScript for why the two are separate. */
struct SupervisionRecord
{
    SupervisionRecordKind kind{SupervisionRecordKind::FREEZE};
    CAsset asset;
    uint256 target;
    XOnlyPubKey old_key;
    std::vector<unsigned char> signature;

    bool IsRotation() const
    {
        return kind == SupervisionRecordKind::ROTATE_OPERATIONAL ||
               kind == SupervisionRecordKind::ROTATE_RECOVERY;
    }
    //! The new key a rotation installs. Meaningless for a freeze.
    XOnlyPubKey NewKey() const { return XOnlyPubKey(target); }
    void SetNewKey(const XOnlyPubKey& key) { target = SupervisionKeyAsTarget(key); }
};

/** Marker prefix identifying a supervision record output. */
extern const std::vector<unsigned char> SUPERVISION_RECORD_MARKER;

/** Build a record's scriptPubKey.
 *
 *      <MARKER> OP_DROP <kind> OP_DROP <asset> OP_DROP <target> OP_DROP
 *      [<old_key> OP_DROP]  <signature> OP_DROP  OP_TRUE
 *
 *  Retained in the UTXO set, like a declaration and for the same reason: the
 *  freeze set has to be a function of the UTXO set. Unlike a declaration this
 *  one ends OP_TRUE rather than OP_RETURN, because a freeze record is meant to
 *  be spent -- spending it is the unfreeze.
 *
 *  The OP_TRUE is the part that looks alarming and is not. The script is NOT
 *  the spend authority; consensus is, and it requires a signature by the
 *  asset's CURRENT operational key (CheckSupervisionRecordSpend). The script
 *  cannot be the authority, and this is forced rather than chosen: a script is
 *  fixed when the output is created, so it can only ever name the key that was
 *  current then. Name it and a rotation leaves every existing freeze liftable
 *  by the old key, which is precisely the trap rotation exists to close
 *  (Alberto's point 2); require the current key on top of it and no key
 *  satisfies both after a rotation, so freezes become permanent and unfreezing
 *  is impossible. There is no third option, so the script contributes nothing
 *  and says so, and the authority lives in the one place that can follow a
 *  rotation. The output carries no value, so a spend that consensus rejects
 *  gains its author nothing but a rejected transaction. */
CScript BuildSupervisionRecordScript(const SupervisionRecord& record);

/** Parse a record output, or nullopt if it is not one. */
std::optional<SupervisionRecord> ParseSupervisionRecordScript(const CScript& script);

/** The message an admission signature covers.
 *
 *  Binds the record's contents AND the first outpoint the creating transaction
 *  spends. The outpoint is what makes the signature single-use: without it, a
 *  freeze record's signature could be lifted off the chain and replayed to
 *  re-freeze a target the issuer had deliberately unfrozen, by anybody. */
uint256 SupervisionRecordSigHash(const SupervisionRecord& record, const COutPoint& first_input);

/** The message that authorises spending a freeze record, i.e. unfreezing.
 *
 *  Binds the record being spent, so the signature cannot be moved to another
 *  record of the same asset. */
uint256 SupervisionUnfreezeSigHash(const COutPoint& record_outpoint, const CAsset& asset,
                                   const uint256& target);

/** Hash of a scriptPubKey, as named by a freeze record.
 *
 *  A hash rather than the script itself so a record is fixed-width whatever it
 *  freezes, and so the record does not restate a script the chain already has. */
uint256 SupervisionTargetHash(const CScript& script);

/** The supervised assets of the active chain, and their freeze state.
 *
 *  A pure function of the UTXO set, exactly like StakeRegistry's UTXO layers
 *  (src/pos.h) and maintained on the same discipline: applied in ConnectTip,
 *  exactly inverted in DisconnectTip, rebuilt by a UTXO scan at startup. That
 *  discipline is the whole reason declarations and records are retained
 *  outputs. A failure to read undo data on disconnect must be fatal, as it is
 *  for the stake registry, or the registry silently disagrees with consensus.
 *
 *  Frozen targets are COUNTED, not held in a set: two records may name the same
 *  target, and the freeze must lift only when the last of them is spent, or the
 *  registry stops being an exact function of the UTXO set under reorg. */
class SupervisionRegistry
{
private:
    struct Entry {
        //! As committed in the asset id, and therefore never changing.
        SupervisionDescriptor descriptor;
        //! Current keys: the descriptor's, advanced by every rotation record.
        XOnlyPubKey operational;
        XOnlyPubKey recovery;
        //! target script hash -> number of unspent freeze records naming it.
        //! The wildcard target SUPERVISION_PAUSE_TARGET lives in here too, so
        //! a pause counts, reverts and rebuilds by exactly the same code as
        //! every other freeze. That reuse is the whole design.
        std::map<uint256, uint32_t> frozen;
    };

    mutable Mutex m_mutex;
    std::map<CAsset, Entry> m_assets GUARDED_BY(m_mutex);

public:
    static SupervisionRegistry& GetInstance()
    {
        static SupervisionRegistry instance;
        return instance;
    }

    void Clear()
    {
        LOCK(m_mutex);
        m_assets.clear();
    }

    //! Nothing supervised anywhere. The short-circuit that keeps this feature
    //! free for chains and blocks that never use it.
    bool Empty() const
    {
        LOCK(m_mutex);
        return m_assets.empty();
    }

    //! Whether any asset has at least one frozen target. Separate from Empty()
    //! because supervised assets with no freezes are the ordinary case, and the
    //! spend path should cost nothing then.
    bool AnyFrozen() const
    {
        LOCK(m_mutex);
        for (const auto& e : m_assets) {
            if (!e.second.frozen.empty()) return true;
        }
        return false;
    }

    void AddAsset(const SupervisionDeclaration& decl)
    {
        LOCK(m_mutex);
        Entry& entry = m_assets[decl.asset];
        entry.descriptor = decl.descriptor;
        entry.operational = decl.descriptor.operational_key;
        entry.recovery = decl.descriptor.recovery_key;
    }

    void RemoveAsset(const CAsset& asset)
    {
        LOCK(m_mutex);
        m_assets.erase(asset);
    }

    bool IsSupervised(const CAsset& asset) const
    {
        LOCK(m_mutex);
        return m_assets.count(asset) > 0;
    }

    std::optional<SupervisionDescriptor> Descriptor(const CAsset& asset) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return std::nullopt;
        return it->second.descriptor;
    }

    //! The key that may sign freezes and unfreezes for this asset right now.
    std::optional<XOnlyPubKey> OperationalKey(const CAsset& asset) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return std::nullopt;
        return it->second.operational;
    }

    //! The key that may sign rotations for this asset right now.
    std::optional<XOnlyPubKey> RecoveryKey(const CAsset& asset) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return std::nullopt;
        return it->second.recovery;
    }

    bool IsFrozen(const CAsset& asset, const uint256& target) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return false;
        // A pause is a freeze on everything, so it answers for every target.
        // Checked here rather than at the call sites so no enforcement path can
        // ask about one script and miss that the whole asset is stopped.
        if (it->second.frozen.count(SUPERVISION_PAUSE_TARGET) > 0) return true;
        return it->second.frozen.count(target) > 0;
    }

    //! Whether the asset as a whole is paused. Distinct from IsFrozen only for
    //! reporting: an RPC has to be able to say WHY a spend is blocked.
    bool IsPaused(const CAsset& asset) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return false;
        return it->second.frozen.count(SUPERVISION_PAUSE_TARGET) > 0;
    }

    //! Whether this asset was issued with the pause capability at all.
    bool PauseAllowed(const CAsset& asset) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return false;
        return (it->second.descriptor.feature_bits & SUPERVISION_FEATURE_PAUSE) != 0;
    }

    void AddFreeze(const CAsset& asset, const uint256& target)
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return;
        it->second.frozen[target]++;
    }

    void SubFreeze(const CAsset& asset, const uint256& target)
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return;
        auto f = it->second.frozen.find(target);
        if (f == it->second.frozen.end()) return;
        if (--f->second == 0) it->second.frozen.erase(f);
    }

    //! Install a rotation. `expect_old` guards the caller's assumption about
    //! which key is being replaced, so a revert that has drifted fails loudly
    //! rather than installing a key nothing authorised.
    bool Rotate(const CAsset& asset, bool operational_role, const XOnlyPubKey& expect_old,
                const XOnlyPubKey& new_key)
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return false;
        XOnlyPubKey& slot = operational_role ? it->second.operational : it->second.recovery;
        if (!(slot == expect_old)) return false;
        slot = new_key;
        return true;
    }

    //! Every supervised asset, for RPC and for tests. Not on the validation path.
    std::map<CAsset, SupervisionDescriptor> Assets() const
    {
        LOCK(m_mutex);
        std::map<CAsset, SupervisionDescriptor> out;
        for (const auto& e : m_assets) out.emplace(e.first, e.second.descriptor);
        return out;
    }

    //! Frozen targets of one asset, with their record counts. RPC and tests.
    std::map<uint256, uint32_t> FrozenTargets(const CAsset& asset) const
    {
        LOCK(m_mutex);
        auto it = m_assets.find(asset);
        if (it == m_assets.end()) return {};
        return it->second.frozen;
    }
};

/** Admit (or refuse) the supervision records a transaction creates.
 *
 *  Consulted from Consensus::CheckTxInputs, so mempool acceptance and
 *  ConnectBlock reach it by the same path and cannot disagree. Refusal is a
 *  consensus failure, not a policy one: a record that reached a block without
 *  a valid signature would freeze someone on nobody's authority.
 *
 *  What is checked: the named asset is supervised; a freeze is signed by its
 *  current operational key and a rotation by its current recovery key; a
 *  rotation names the key it actually replaces and installs a different, valid
 *  one. Note the asymmetry, which is Alberto's point 2: the operational key
 *  cannot rotate anything, not even itself, so a thief who has it can grief
 *  but can never seize the authority. */
bool CheckSupervisionRecords(const CTransaction& tx, const SupervisionRegistry& registry,
                             std::string& err);

/** Authorise spending a freeze record, i.e. lifting the freeze.
 *
 *  THE UNFREEZE TRAP (Alberto's point 2), and the reason this is a consensus
 *  check rather than something the record's own script could do: validity is
 *  judged against the registry's CURRENT operational key. After a rotation the
 *  old key lifts nothing, which is the entire point of rotating after a
 *  compromise. */
bool CheckSupervisionRecordSpend(const CTxIn& input, const CTxOut& record_out,
                                 const SupervisionRegistry& registry, std::string& err);

/** Whether input `n` spends a script only one party can satisfy.
 *
 *  THE RULE THAT KEEPS A FREEZE FROM TAKING HOSTAGES, and the reason supervised
 *  assets can be used on Lightning and on the DEX at all.
 *
 *  A freeze binds only single-owner scripts. Everything else -- a Lightning
 *  channel's 2-of-2, an HTLC, a covenant, any P2WSH or script-path spend -- is
 *  out of reach, because the frozen party is not the only party to those funds.
 *  Blocking such a spend would strand a counterparty who did nothing, and worse,
 *  freeze a contract whose timelocks keep running: the innocent side would lose
 *  the race it did not know it was in. Consensus cannot freeze a Lightning
 *  balance and should not pretend to. What it CAN do is what Ethereum does,
 *  which is the parity bar: an address holding the asset outright is freezable,
 *  and shared state is not. Breaking a blacklisted holder's channels cleanly is
 *  the Lightning implementation's job, at the layer that knows whose channel it
 *  is.
 *
 *  Decidable at spend time, which is the other half of why it works: an address
 *  only commits to a script's hash, but spending it reveals the script.
 *
 *  Be conservative. Anything unrecognised is NOT single-owner. A false negative
 *  means a freeze does not bite; a false positive destroys somebody's contract
 *  funds. */
bool IsSingleOwnerSpend(const CTransaction& tx, unsigned int n, const CScript& script_pubkey);

/** Whether this output is a supervision declaration or record, correctly
 *  denominated in the asset it names.
 *
 *  Used for one thing: VerifyAmounts otherwise refuses every SPENDABLE
 *  zero-value output, because a zero-value output of a reissuance token would
 *  let anyone reissue an asset without holding its tokens. Supervision outputs
 *  must be zero-valued (an issuer must not burn value into a script only
 *  consensus can release) and must NOT be provably unspendable (the whole
 *  supervision state is read back out of the UTXO set), so they need a carve-out
 *  from that rule.
 *
 *  The carve-out is only safe because of where it is applied: Consensus::
 *  CheckTxInputs vets every declaration and record BEFORE VerifyAmounts runs,
 *  and only admits ones naming an asset the registry already knows is
 *  supervised. A registry asset is never a reissuance token, so the inflation
 *  the original rule prevents stays prevented. Applying it anywhere the vetting
 *  has not happened -- below the activation height, in particular -- would
 *  reopen exactly that hole, which is why VerifyAmounts takes the gate as an
 *  argument rather than reading the script alone. */
bool IsSupervisionOutput(const CTxOut& out);

/** Whether a transaction is one a newly-confirmed record could have invalidated.
 *
 *  The failure this exists to prevent is specific and lethal. A spend that was
 *  valid on entry to the mempool becomes invalid the moment a freeze confirms,
 *  and nothing evicts it: removeForBlock drops only included and directly
 *  conflicting transactions, and a freeze record conflicts with nothing. The
 *  stale entry then stays resident for its full expiry, is re-selected into
 *  every block template, and fails TestBlockValidity every time -- so every
 *  producer skips its slot and THE CHAIN STOPS MAKING BLOCKS.
 *
 *  Three things become stale this way, and all three are checked here: a spend
 *  of a script a new record froze; a record whose admission signature was
 *  written against a key a new rotation has retired; and an unfreeze signed by
 *  that same retired key.
 *
 *  `view` must resolve the transaction's inputs, including outputs created by
 *  other mempool entries, or a child of an evicted parent is missed. */
bool SupervisionInvalidates(const CTransaction& tx, CCoinsView& view,
                            const SupervisionRegistry& registry);

/** Whether a validation failure was one supervision caused.
 *
 *  Used by CTxMemPool::check, whose assert would otherwise turn an issuer's
 *  freeze into a remote node crash under -checkmempool. */
bool IsSupervisionRejection(const std::string& reject_reason);

/** Mirror a connected block's declarations and records into the registry. */
void SupervisionApplyBlock(const CBlock& block, const CBlockUndo& undo);

/** Exact inverse of SupervisionApplyBlock, for tip disconnects. */
void SupervisionRevertBlock(const CBlock& block, const CBlockUndo& undo);

/** Rebuild the registry by scanning the (flushed) UTXO set. */
bool RebuildSupervisionRegistry(CCoinsView& view);

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
