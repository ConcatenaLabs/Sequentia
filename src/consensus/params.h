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
    //! SENTINEL ASYMMETRY, mind the neighbour above: here 0 DISABLES the rule
    //! (PosExpRaceActive tests pos_exprace_height > 0), whereas 0 in
    //! pos_coinbase_leader_height means ACTIVE FROM GENESIS. A chain that wants
    //! the exp-race from its first elected block therefore sets 1, not 0, which
    //! is what the Sequentia mainnet chain does; the guard is also what keeps the
    //! fork off by default on regtest and custom chains, so do not weaken it.
    int pos_exprace_height{0};
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
    //! SEQUENTIA: block height from which supervised assets exist (src/supervision.h).
    //!
    //! Same convention as pos_exprace_height above: 0 = rule off, a positive H
    //! = active from height H. A chain launched WITH the rule sets 1.
    //!
    //! This gate is not the usual courtesy to old history. Consensus DERIVES an
    //! asset id rather than reading it from the transaction, so below H a
    //! supervision declaration is inert data and the issuance derives plainly,
    //! which is exactly what a node without this code does. Above H the same
    //! issuance derives a different asset. The two behaviours cannot coexist on
    //! one chain, so a supervised asset cannot be issued before every node has
    //! crossed H. Set H above the tip at release and cut every node over at
    //! once, as with the Simplicity activation.
    //!
    //! One height, not two: the same value gates the derivation and, later, the
    //! freeze enforcement built on it. Splitting them would allow a window in
    //! which supervised assets exist but cannot be supervised, which is the one
    //! state the feature must never be in.
    int supervised_assets_height{0};
    //! SEQUENTIA PoS: the MINIMUM SECONDS between a block and its parent,
    //! enforced by consensus. 0 = no minimum (the rule is off).
    //!
    //! Why this exists. The chain's 30-second cadence lives in
    //! PosProducer::Step as a *producer-side* floor, and no validator checks
    //! it: a modified producer ignores it and every node accepts the result.
    //! The slot gate is no substitute — it scales with the reciprocal of a
    //! staker's weight, so it can never be a uniform speed limit, and the
    //! winning draw floors to 0 in ~63% of rounds, which is exactly when it
    //! would need to bind. Measured: a hostile set holding all the stake can
    //! drive the chain to a 17.5 s cadence today, and would reach 0.6 s if the
    //! gate were rescaled to one second per score unit.
    //!
    //! Producers collect the fees and nothing else -- Sequentia has no block
    //! subsidy at all (genesis_subsidy is 0 on both real chains; whitepaper
    //! §3.9, no inflation) -- so accelerating buys them fee-bearing blocks,
    //! and the incentive is permanent and grows with congestion. More blocks
    //! in the same time is more disk, more bandwidth and more validation for
    //! every node forever, which is the opposite of what this chain is for.
    //! Note that emission is NOT at stake here, precisely because there is
    //! none: this is a resource-consumption rule, not a monetary one.
    //!
    //! Why a timestamp rule is enough. A validator cannot observe when a block
    //! really appeared, only the time WRITTEN in it, so the written times are
    //! the only thing a consensus rule can bind — and binding them suffices:
    //! producing N blocks costs N * spacing seconds of timestamp, and
    //! MAX_FUTURE_BLOCK_TIME caps how far ahead of real time those timestamps
    //! may run. The two together bound the long-run rate at exactly one block
    //! per `pos_block_spacing` seconds, after a one-off burst of at most
    //! MAX_FUTURE_BLOCK_TIME / pos_block_spacing blocks that buys nothing
    //! because the chain then stalls until real time catches up.
    //!
    //! Deliberately SEPARATE from the slot-gate unit (g_pos_slot_interval).
    //! They answer different questions — "how fast may the chain run" versus
    //! "in what order may leaders propose" — and folding them into one number
    //! is what made every choice of that number a bad trade. Raising the
    //! spacing must not silently stretch the slot gate too.
    //!
    //! This is a consensus parameter and NOT a global read from an argument,
    //! unlike g_pos_slot_interval: two operators starting with different
    //! -posslotinterval values disagree about which blocks are valid, which is
    //! a chain split waiting for a trigger rather than a configuration choice.
    int64_t pos_block_spacing{0};
    //! SEQUENTIA PoS: block height from which pos_block_spacing binds.
    //!
    //! Same convention as pos_escape_stall_mtp_height above: a chain launched
    //! WITH the rule sets 1, never 0. On the running testnet the value must be
    //! ABOVE the tip at release time: 2,186 of its first 86,357 blocks sit
    //! closer than 30 s to their parent (2,183 of them at exactly 29 s, a
    //! one-second clock skew between producers, still occurring at the tip), so
    //! a rule applied retroactively would make the chain unsyncable.
    //!
    //! Before activating this on a running chain, ship the producer-side clamp
    //! FIRST and confirm that no node still emits a block closer than the
    //! spacing to its parent. The clamp is what turns those 29-second blocks
    //! into 30-second ones; without it this rule invalidates blocks that honest
    //! producers are emitting right now.
    //!
    //! Changing when a block is valid is a HARD FORK: coordinate H with every
    //! operator (see doc/sequentia/04-proof-of-stake.md).
    int pos_block_spacing_height{0};
    //! SEQUENTIA PoS: seconds of leader time-gate per unit of sortition score,
    //! and the height it binds from. 0 = keep the historic unit, which is the
    //! runtime global g_pos_slot_interval.
    //!
    //! The gate delays a leader by `floor(score) x unit` after the parent, and
    //! the producer will not propose before pos_block_spacing in any case, so
    //! every draw whose gate lands under the cadence proposes at the same
    //! moment. The unit therefore decides how much of the draw distribution the
    //! cadence absorbs -- and with the exponential race the tail is what costs
    //! throughput. Measured over 2,000,000 simulated rounds at a 60 s cadence
    //! with twelve equal stakers:
    //!
    //!     unit 30 s -> 4.97% of blocks late, 3.78% throughput lost, field 2.7
    //!     unit 10 s -> 0.09% of blocks late, 0.02% throughput lost, field 5.3
    //!
    //! Ten seconds takes 99.4% of the available gain. Below it the throughput
    //! is already recovered and only the field keeps widening, which buys
    //! nothing and gives the anchor-freshness key more material to reorder.
    //!
    //! DELIBERATELY NOT g_pos_slot_interval, which stays at 30. That global is
    //! also the axis PosRequiredUnbondingSeconds() and PosStakeLockSeconds()
    //! use to put height- and time-based stake locks on one scale, so lowering
    //! it to 10 would silently cut the unbonding requirement from ~15 days to
    //! ~5. Two jobs, two numbers.
    //!
    //! Only meaningful under the exponential-race election: the legacy slot is
    //! a bounded RANK (uniform in [0, W/w)), for which the whole-interval scale
    //! is the whitepaper's rank-r liveness gate and costs nothing.
    //!
    //! Changing when a leader may produce is a HARD FORK: coordinate H with
    //! every operator.
    int64_t pos_slot_gate_seconds{0};
    int pos_slot_gate_height{0};
    //! Whether the fine leader time-gate is the rule at `height`.
    bool PosSlotGateActiveAt(int height) const
    {
        return pos_slot_gate_seconds > 0 && pos_slot_gate_height > 0 &&
               height >= pos_slot_gate_height;
    }
    //! SEQUENTIA: coinbase maturity in blocks, and the height it binds from.
    //! 0 = use the inherited COINBASE_MATURITY (consensus.h).
    //!
    //! COINBASE_MATURITY is a number of BLOCKS, so what it actually protects
    //! shrinks as a chain's cadence shortens: Bitcoin's 100 blocks at 600 s is
    //! 16 h 40 min, while the same 100 blocks at 60 s is 100 minutes. Sequentia
    //! holds the WALL-CLOCK figure equal to Bitcoin's rather than the block
    //! count, so the number here must satisfy
    //!
    //!     coinbase_maturity * pos_block_spacing == 100 * 600
    //!
    //! and has to be revisited whenever the cadence is. It matters more here
    //! than upstream: there is no block subsidy (§3.9), so a coinbase carries
    //! the producer's fee income rather than new issuance, and the maturity is
    //! the delay before a producer can spend what it earned.
    //!
    //! Raising it REJECTS MORE, hence the height: the running testnet has
    //! coinbases already spent at depths between 100 and 1,000, and applying
    //! the new figure to them would make the chain unsyncable.
    int coinbase_maturity{0};
    int coinbase_maturity_height{0};
    //! Whether the minimum-spacing rule binds a block at `height`.
    //! Consulted by ContextualCheckBlockHeader and by the producer, so the two
    //! can never disagree about how early a block may be stamped.
    bool PosBlockSpacingActiveAt(int height) const
    {
        return pos_block_spacing > 0 && pos_block_spacing_height > 0 &&
               height >= pos_block_spacing_height;
    }
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
    //! SEQUENTIA: height from which a Simplicity witness byte buys
    //! SIMPLICITY_BUDGET_PER_WITNESS_BYTE weight units of execution budget
    //! rather than one. 0 = in force from genesis.
    //!
    //! The rule itself needs no activation gate: it only ever accepts more, so
    //! no previously accepted block can fail under it (CONTRIBUTING.md). What
    //! the gate buys is a FLAG DAY. Without one the fork is unscheduled -- it
    //! fires the instant anyone broadcasts a spend that the old budget could not
    //! pay for, and every node still on the old rule rejects that block and
    //! forks off. With one, the new binary keeps enforcing the old budget until
    //! H, so nobody can trigger the split early and every operator has a date.
    int simplicity_budget4_height{0};
    //! Genesis hash of the chain the height above was written for.
    uint256 simplicity_budget4_chain_genesis;
    //! Whether the wider Simplicity budget is in force at `height`.
    //!
    //! Both gates again, and for the reason UtxoRecoveryAppliesAt gives: a fresh
    //! chain -- regtest, a re-genesised testnet, a future mainnet -- has no
    //! history and no other operators to coordinate with, so it should have the
    //! rule from genesis rather than inherit a flag day meant for somebody
    //! else's running network. Binding to the genesis hash is what makes a
    //! re-genesis DROP the delay instead of waiting out a height that no longer
    //! means anything.
    bool SimplicityBudget4ActiveAt(int height) const
    {
        if (simplicity_budget4_height == 0) return true;
        if (simplicity_budget4_chain_genesis != hashGenesisBlock) return true;
        return height >= simplicity_budget4_height;
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
