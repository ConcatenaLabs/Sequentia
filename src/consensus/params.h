// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include <asset.h>
#include <optional>
#include <uint256.h>
#include <limits>
#include <utility>
#include <vector>

#include <script/script.h> // mandatory_coinbase_destination
#include <consensus/amount.h> // genesis_subsidy

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_CLTV,
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CSV,
    DEPLOYMENT_SEGWIT,
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_SEGWIT; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    DEPLOYMENT_TAPROOT, // Deployment of Schnorr/Taproot (BIPs 340-342)
    DEPLOYMENT_DYNA_FED, // Deployment of dynamic federation
    DEPLOYMENT_SIMPLICITY, // Deployment of Simplicity
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return DEPLOYMENT_TESTDUMMY <= dep && dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit;
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    // ELEMENTS: Interpreted as block height!
    int64_t nStartTime;
    /** Timeout/expiry MedianTime for the deployment attempt. */
    // ELEMENTS: Interpreted as block height!
    int64_t nTimeout;
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};

    // ELEMENTS: allow overriding the signalling period length rather than using `nMinerConfirmationWindow`
    std::optional<uint32_t> nPeriod{std::nullopt};
    // ELEMENTS: allow overriding the activation threshold rather than using `nRuleChangeActivationThreshold`
    std::optional<uint32_t> nThreshold{std::nullopt};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/** SEQUENTIA: one output created by a UtxoRecovery (see below). */
struct UtxoRecoveryOutput {
    CAsset asset;
    CAmount amount{0};
    CScript scriptPubKey;
};

/**
 * SEQUENTIA: a one-time, chain-specific, deterministic rewrite of the UTXO set,
 * applied by the block-connect path at a single fixed height.
 *
 * THIS IS NOT A MECHANISM TO REUSE. It exists because of one specific accident
 * and it is scoped to that accident: read the comment on
 * CTestNetParams::consensus.utxo_recovery in chainparams.cpp for what happened,
 * why the owner authorised it, and what it costs. Anything that can be done with
 * an ordinary transaction MUST be done with an ordinary transaction. A rewrite
 * moves coins that no signature authorised, so the only thing that makes one
 * legitimate is that every node applies exactly the same one, and that its
 * contents are auditable in the source.
 *
 * The shape, deliberately: a set of outpoints to RETIRE (remove from the UTXO
 * set) and a set of outputs to CREATE. The created outputs are placed at the
 * outpoints of a deterministic synthetic transaction built from this table
 * (BuildUtxoRecoveryTransaction, validation.cpp), so their txid is a pure
 * function of the table and anyone can recompute it. They are ordinary coins
 * from that moment on: spending one needs a signature satisfying its
 * scriptPubKey, with no special case anywhere in the spend path.
 *
 * Applied ALL-OR-NOTHING: if any retired outpoint is not present and unspent,
 * nothing at all happens. That is what makes the rule safe to run on every node
 * unconditionally -- a node whose UTXO set does not contain the coins (because
 * it is a different chain, or because someone edited the table) simply carries
 * on, rather than stalling or splitting the network.
 *
 * Gating (see Params::UtxoRecoveryAppliesAt): the table binds to ONE chain by
 * genesis hash as well as by height. A fresh chain -- regtest, a re-genesised
 * testnet, mainnet -- has an empty table and must never inherit this one. New
 * chains carry no one else's accident.
 */
struct UtxoRecovery {
    //! Height of the block whose connection applies the rewrite. 0 = no rewrite
    //! on this chain, which is the default every chain gets.
    int height{0};
    //! The genesis hash of the chain this rewrite belongs to. Compared against
    //! Params::hashGenesisBlock, so the table disables itself if it is ever
    //! carried onto a chain it was not written for.
    uint256 chain_genesis;
    //! Outpoints removed from the UTXO set: (txid, vout).
    std::vector<std::pair<uint256, uint32_t>> retire;
    //! Outputs added to the UTXO set.
    std::vector<UtxoRecoveryOutput> create;

    bool IsNull() const { return height <= 0 || retire.empty(); }
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    int nSubsidyHalvingInterval;
    /* Block hash that is excepted from BIP16 enforcement */
    uint256 BIP16Exception;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which Segwit (BIP141, BIP143 and BIP147) becomes active.
     * Note that segwit v0 script rules are enforced on all blocks except the
     * BIP 16 exception blocks. */
    int SegwitHeight;
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV and segwit activations. */
    int MinBIP9WarningHeight;
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    uint32_t nMinerConfirmationWindow;
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];
    /** Proof of work parameters */
    uint256 powLimit;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    /** The best chain should have at least this much work */
    uint256 nMinimumChainWork;
    /** By default assume that the signatures in ancestors of this block are valid */
    uint256 defaultAssumeValid;

    /**
     * If true, witness commitments contain a payload equal to a Bitcoin Script solution
     * to the signet challenge. See BIP325.
     */
    bool signet_blocks{false};
    std::vector<uint8_t> signet_challenge;

    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_SEGWIT:
            return SegwitHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }

    //
    // ELEMENTS CHAIN PARAMS
    CScript mandatory_coinbase_destination;
    //! SEQUENTIA PoS: block height from which a con_pos block's coinbase must pay
    //! every fee-bearing output to the elected leader's own key (P2WPKH of the
    //! challenge leader). 0 = enforce from genesis (the Sequentia mainnet
    //! default). On an already-running chain that produced pre-rule blocks paying
    //! fees to the anyone-can-spend fallback, set this above the existing tip so
    //! those historical blocks are grandfathered while the rule binds all blocks
    //! from H onward. See doc/sequentia/04-proof-of-stake.md.
    int pos_coinbase_leader_height{0};
    //! SEQUENTIA PoS: block height from which leader election uses the
    //! exponential-race (weighted-sampling) sortition (PosVrfSlotExp /
    //! PosVrfScoreExp) instead of the legacy PosVrfSlot / raw-beta election.
    //! The exp-race is exactly stake-proportional and split-neutral; switching
    //! to it changes which block wins, so it is a HARD FORK gated by height.
    //! 0 = disabled (keep the legacy election); a positive H activates it from
    //! height H on every node at once. Set per chain; coordinate the value with
    //! all operators before it is reached (see doc/sequentia/04-proof-of-stake.md).
    int pos_exprace_height{0};
    //! SEQUENTIA PoS: block height from which the exponential-race leader
    //! time-gate is measured in SECONDS of score instead of whole slot
    //! intervals (PosSlotGateSeconds, pos.h). Only meaningful where the
    //! exp-race election is itself active.
    //!
    //! Why the rule exists. The legacy PosVrfSlot is a RANK: slot is uniform in
    //! [0, W/w), so a staker with share s always draws slot < 1/s and the best
    //! draw on any chain with a substantial staker is 0 or 1. Multiplying that
    //! rank by the slot interval is the whitepaper's rank-r liveness gate and
    //! costs the chain nothing. The exponential-race score is NOT a rank: it is
    //! -ln(U)·W/w, and the MINIMUM over all stakers is Exponential(1) — mean 1,
    //! with an unbounded geometric tail. Multiplying THAT by the whole slot
    //! interval taxes the live winner: the network stays silent for
    //! floor(min score) intervals, so P(a block is late by at least one whole
    //! interval) = e^-2 ~ 13.5%, and the mean interval is 30·E[max(1,floor(X))]
    //! = ~1.21 intervals. Measured on the Sequentia testnet across the
    //! pos_exprace_height fork: 0.0% late and a 30.00 s mean below it, 15.2%
    //! late and a 38.45 s mean above it, with every late interval an exact
    //! multiple of the slot interval.
    //!
    //! The fix keeps the score as the ordering key (it is the election) and only
    //! changes the SCALE of the time gate, to one second per score unit: the
    //! winner is gated at ~1 s (so the producer's cadence floor of one interval
    //! decides, as it did before the exp-race fork), while a candidate scoring N
    //! units worse still cannot produce until N seconds later, so the fallback
    //! ordering the gate exists for survives. See doc/sequentia/04-proof-of-stake.md.
    //!
    //! Same convention as pos_exprace_height above: 0 = not gated (rule off),
    //! a positive H = enforced from height H. Changing when a leader may
    //! produce is a HARD FORK: coordinate H with every operator.
    int pos_exprace_gate_height{0};
    //! SEQUENTIA PoS: block height from which the escaping-stall PARENT-CHAIN
    //! MEDIAN-TIME-PAST gap (CheckEscapingStallMtpGap, anchor.h) is enforced.
    //!
    //! Same convention as pos_exprace_height above: 0 = not gated (rule off),
    //! a positive H = enforced from height H on, leaving earlier history exempt.
    //! A chain launched WITH the rule sets 1 ("active from the first block"),
    //! never 0 — see CONTRIBUTING.md, "Every new consensus rule needs an
    //! activation height". Keeping every gate on one convention matters: two
    //! gates with opposite meanings for 0 would make a reviewer "fixing" a 0
    //! silently disable a consensus rule.
    //!
    //! Why this gate exists. The MTP-gap requirement was added in response to
    //! the 2026-07-17 finality partition, i.e. AFTER the testnet chain had
    //! already produced blocks that do not satisfy it (the earliest observed is
    //! testnet height 1757, dated 2026-07-06). Because it was enforced from
    //! genesis with no activation height, those blocks became unvalidatable:
    //! a node syncing from scratch stops there for ever and can never join the
    //! network, and an existing node survives only because blocks already in
    //! its chainstate are never re-validated — so a -reindex, a restore from
    //! backup or any resync would leave it unable to start. Gating the rule by
    //! height is the standard soft-fork treatment and fixes both.
    //!
    //! Choosing the value: it must be ABOVE the chain tip at the time the
    //! binary is released, so no node can disagree about already-existing
    //! blocks. Below H the rule is simply not applied, which is exactly the
    //! behaviour every already-synced node has today.
    int pos_escape_stall_mtp_height{0};
    //! SEQUENTIA: the one-time UTXO-set rewrite this chain applies, if any.
    //! Empty (height 0) on every chain but the one it was written for -- see the
    //! UtxoRecovery comment above and CTestNetParams in chainparams.cpp.
    UtxoRecovery utxo_recovery;
    //! Whether the block at `height` is the one that applies the UTXO rewrite.
    //!
    //! Both gates must hold: the height, and the genesis hash of the chain the
    //! rewrite was written for. The genesis gate is what stops a fresh chain --
    //! regtest, a re-genesised testnet, a future mainnet -- from inheriting
    //! someone else's one-time intervention just because it reached the same
    //! height. Consulted identically by ConnectBlock and DisconnectBlock, so the
    //! two can never disagree about whether a block carries the rewrite.
    bool UtxoRecoveryAppliesAt(int height) const
    {
        return !utxo_recovery.IsNull()
            && height == utxo_recovery.height
            && utxo_recovery.chain_genesis == hashGenesisBlock;
    }
    CAmount genesis_subsidy;
    //! SEQUENTIA: per-chain maximum block weight (BIP141 weight units). 0 means
    //! "use the global MAX_BLOCK_WEIGHT". Sequentia sets this to 200,000 (a
    //! twentieth of Bitcoin's 4,000,000) so that, at ~30-second blocks, a
    //! saturated Sequentia chain grows at the same total rate as a saturated
    //! Bitcoin chain (whitepaper §3.10).
    uint32_t nMaxBlockWeight{0};
    CAsset subsidy_asset;
    bool connect_genesis_outputs;
    bool has_parent_chain;
    uint256 parentChainPowLimit;
    uint32_t pegin_min_depth;
    CScript parent_chain_signblockscript;
    bool ParentChainHasPow() const { return parent_chain_signblockscript == CScript();}
    CScript fedpegScript;
    CAsset pegged_asset;
    CAsset parent_pegged_asset;
    // g_con_blockheightinheader global hack instead of proper arg due to circular dep
    std::string genesis_style;
    CScript signblockscript;
    uint32_t max_block_signature_size;
    // g_signed_blocks - Whether blocks are signed or not, get around circular dep
    // Set positive to avoid division by 0
    // for non-dynafed chains and unit tests
    uint32_t dynamic_epoch_length = std::numeric_limits<uint32_t>::max();
    // Used to seed the extension space for first dynamic blocks
    std::vector<std::vector<unsigned char>> first_extension_space;
    // Used to allow M-epoch-old peg-in addresses as deposits
    // default 1 to not break legacy chains implicitly.
    size_t total_valid_epochs = 1;
    bool elements_mode = false;
    bool start_p2wsh_script = false;
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
