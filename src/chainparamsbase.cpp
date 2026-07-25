// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>

#include <tinyformat.h>
#include <util/system.h>

#include <assert.h>

const std::string CBaseChainParams::MAIN = "main";
const std::string CBaseChainParams::SEQUENTIA = "sequentia";
const std::string CBaseChainParams::TESTNET = "test";
const std::string CBaseChainParams::SIGNET = "signet";
const std::string CBaseChainParams::REGTEST = "regtest";
const std::string CBaseChainParams::LIQUID1 = "liquidv1";
const std::string CBaseChainParams::LIQUID1TEST = "liquidv1test";
const std::string CBaseChainParams::LIQUIDTESTNET = "liquidtestnet";

// SEQUENTIA: this build targets the Sequentia testnet ("test" / CTestNetParams),
// which is the chain the live committee, gateway and explorer all run. Default to
// it so the GUI, daemon and CLI all connect to the testnet without -chain.
// (For a mainnet release build, set this to CBaseChainParams::SEQUENTIA.)
const std::string CBaseChainParams::DEFAULT = CBaseChainParams::TESTNET;

void SetupChainParamsBaseOptions(ArgsManager& argsman)
{
    argsman.AddArg("-chain=<chain>", "Use the chain <chain> (default: test, the Sequentia testnet). Reserved values: main, test, signet, regtest, liquidv1, liquidv1test, liquidtestnet", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-regtest", "Enter regression test mode, which uses a special chain in which blocks can be solved instantly. "
                 "This is intended for regression testing tools and app development. Equivalent to -chain=regtest.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testactivationheight=name@height.", "Set the activation height of 'name' (segwit, bip34, dersig, cltv, csv). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testnet", "Use the test chain. Equivalent to -chain=test.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-vbparams=deployment:start:end[:min_activation_height]", "Use given start/end times and min_activation_height for specified version bits deployment (regtest or custom only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-seednode=<ip>", "Use specified node as seed node. This option can be specified multiple times to connect to multiple nodes. (custom only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);

    argsman.AddArg("-signet", "Use the signet chain. Equivalent to -chain=signet. Note that the network is defined by the -signetchallenge parameter", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetchallenge", "Blocks must satisfy the given script to be considered valid (only for signet networks; defaults to the global default signet test network challenge)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetseednode", "Specify a seed node for the signet network, in the hostname[:port] format, e.g. sig.net:1234 (may be used multiple times to specify multiple seed nodes; defaults to the global default signet test network seed node(s))", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);

    //
    // ELEMENTS
    argsman.AddArg("-con_mandatorycoinbase", "All non-zero valued coinbase outputs must go to this scriptPubKey, if set.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_blocksubsidy", "Defines the amount of block subsidy to start with, at genesis block, in satoshis.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_connect_genesis_outputs", "Connect outputs in genesis block to utxo database.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_elementsmode", "Use Elements-like instead of Core-like witness encoding.  This is required for CA/CT. (default: 1)", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_blockheightinheader", "Whether the chain includes the block height directly in the header, for easier validation of block height in low-resource environments. (default: 1)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_genesis_style=<style>", "Use genesis style <style> (default: elements). Results in genesis block compatibility with various networks. Allowed values: elements, bitcoin", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_signed_blocks", "Signed blockchain. Uses input of `-signblockscript` to define what signatures are necessary to solve it.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signblockscript", "Signed blockchain enumberance. Only active when `-con_signed_blocks` set to true.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_max_block_sig_size", "Max allowed witness data for the signed block header.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_pos", "SEQUENTIA: enable Proof-of-Stake leader election. Each block must be signed by the stake-weighted leader elected for its slot from a seed derived from the previous block and its Bitcoin anchor. Implies signed blocks and disables dynamic federations. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-staker=<pubkeyhex:weight>", "SEQUENTIA: register a staker public key and its stake weight for Proof-of-Stake leader election. May be specified multiple times. Only used when -con_pos is set.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posslotinterval=<n>", "SEQUENTIA: seconds per leader rank (also the chain's nominal block time); the rank-r leader of a slot may produce a block only once n*r seconds have elapsed since the parent block. Only used when -con_pos is set. (default: 30)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posvrf", "SEQUENTIA: use private VRF sortition for Proof-of-Stake leader election: each block carries the leader's VRF proof over the slot seed in a tagged coinbase OP_RETURN, and the proof output determines the leader's time-gated slot. Requires -con_pos. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posunbonding=<n>", "SEQUENTIA: minimum CSV unbonding delay, in blocks, for an output to count as on-chain stake (the staking script's relative timelock must be at least this). Only used when -con_pos is set. (default: 10)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-pospayoutnotice=<n>", "SEQUENTIA: notice period, in blocks, before a block producer's newly announced payout policy may take effect. Lets delegators audit a pool and leave before a hostile change binds. Only used when -con_pos is set. (default: 2880)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_genesis_stake=<pubkeyhex:atoms:csv>", "SEQUENTIA: seed a genesis staking output of the policy asset (a CSV-locked stake for <pubkey>), making it the sole genesis staker so the chain bootstraps PoS with no -staker config. The spendable remainder is set via -initialfreecoins. Only used when -con_pos is set on a custom chain.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-poscommitteesize=<n>", "SEQUENTIA: number of committee members (the first n stakers of each slot's leader schedule) that certify each block; a strict majority must countersign. 1 disables committee certification (leader-only signing). Range 1-16, or 1-100 with -posaggcommittee. Only used when -con_pos is set. (default: 1)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posaggcommittee", "SEQUENTIA: certify blocks with a single MuSig2 (BIP340) aggregate signature of the VRF-sortitioned committee instead of script multisig, lifting the committee-size cap from 16 to 100 (the paper's scale). Requires -posvrf. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posbls", "SEQUENTIA: certify blocks with a single non-interactive BLS12-381 aggregate signature of the VRF-sortitioned committee (each member's BLS key derived from its staking key, with the BLS pubkey and proof-of-possession committed in the coinbase). Supersedes -posaggcommittee. Requires -posvrf. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-pospubliccommittee", "SEQUENTIA: public fixed-size committee (impl spec Option A). Committee membership is the first min(#stakers, -poscommitteesize) entries of the deterministic public schedule instead of private threshold VRF sortition, and the certification quorum derives from that ACTUAL size (strict majority, plus one at odd sizes), so the eligible set can never exceed the cap and two disjoint quorums cannot certify rival same-height blocks. Leader election stays private-VRF. NETWORK-WIDE consensus rule: every node on a chain must agree. Requires -posbls. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posminstake=<n>", "SEQUENTIA: minimum stake weight, in policy-asset atoms, a key must hold to be an eligible blocksigner (leader or committee member); stake below this is ignored by election and sortition (whitepaper §3.3: 0.01% of supply = 40,000 SEQ). 0 disables the floor. Only used when -con_pos is set. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posexpraceheight=<n>", "SEQUENTIA: block height from which leader election uses the exponential-race (weighted-sampling) sortition instead of the legacy raw-beta election. The exp-race is exactly stake-proportional and split-neutral; the switch changes which block wins, so it is a coordinated HARD FORK. 0 disables it. Custom/regtest chains only (the real chains pin it in code). Only used when -con_pos is set. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);

    argsman.AddArg("-con_default_blinded_addresses", "SEQUENTIA: whether wallets give blinded (confidential) addresses by default on this chain. 1 = historical Liquid/Elements behavior (CT opt-out); 0 = Sequentia behavior (CT opt-in; the default address format matches Bitcoin's). Custom chains only. (default: 1)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_maxblockweight=<n>", "SEQUENTIA: per-chain maximum block weight in BIP141 weight units; 0 uses the global maximum. Sequentia uses 200000 (a twentieth of Bitcoin's, for ~30s blocks; whitepaper §3.10). Custom chains only. (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_has_parent_chain", "Whether or not there is a parent chain.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-parentgenesisblockhash", "The genesis blockhash of the parent chain.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_parentpowlimit", "The proof-of-work limit value for the parent chain.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-con_parent_chain_signblockscript", "Whether parent chain uses pow or signed blocks. If the parent chain uses signed blocks, the challenge (scriptPubKey) script. If not, an empty string. (default: empty script [ie parent uses pow])", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);

    argsman.AddArg("-fedpegscript", "The script for the federated peg enforce from genesis block. This script may stop being enforced once dynamic federations activates.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-enforce_pak", "Causes standardness checks to enforce Pegout Authorization Key(PAK) validation before dynamic federations, and consensus enforcement after.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-pak", "Sets the 'first extension space' field to the pak entries ala pre-dynamic federations. Only used for testing in custom chains.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-multi_data_permitted", "Allow relay of multiple OP_RETURN outputs. (default: -enforce_pak)", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_csv_deploy_start", "Starting height for CSV deployment. (default: -1, which means ACTIVE from genesis)", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_dyna_deploy_signal", "Whether to signal for the Dynamic Federations deployment (default: 1).", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-dynamic_epoch_length", "Per-chain parameter that sets how many blocks dynamic federation voting and enforcement are in effect for.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-total_valid_epochs", "Per-chain parameter that sets how long a particular fedpegscript is in effect for.", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-evbparams=deployment:start:end:period:threshold", "Use given start/end times for specified version bits deployment (regtest or custom only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-con_start_p2wsh_script", "Create p2wsh addresses when starting in dynafed mode (regtest or custom only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::ELEMENTS);
    argsman.AddArg("-acceptunlimitedissuances", "Allow unblinded issuance amounts to exceed 21 million units", ArgsManager::ALLOW_ANY, OptionsCategory::ELEMENTS);
    // END ELEMENTS
    //
}

static std::unique_ptr<CBaseChainParams> globalChainBaseParams;

const CBaseChainParams& BaseParams()
{
    assert(globalChainBaseParams);
    return *globalChainBaseParams;
}

/**
 * Port numbers for incoming Tor connections (8334, 18334, 38334, 18445) have
 * been chosen arbitrarily to keep ranges of used ports tight.
 */
std::unique_ptr<CBaseChainParams> CreateBaseChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN) {
        return std::make_unique<CBaseChainParams>("", 8332, 18332, 8334);
    } else if (chain == CBaseChainParams::SEQUENTIA) {
        return std::make_unique<CBaseChainParams>("sequentia", 7332, 18332, 7334);
    } else if (chain == CBaseChainParams::TESTNET) {
        return std::make_unique<CBaseChainParams>("testnet3", 18776, 18332, 18778);
    } else if (chain == CBaseChainParams::SIGNET) {
        return std::make_unique<CBaseChainParams>("signet", 38332, 18332, 38334);
    } else if (chain == CBaseChainParams::REGTEST) {
        return std::make_unique<CBaseChainParams>("regtest", 18443, 18332, 18445);
    } else if (chain == CBaseChainParams::LIQUID1) {
        return std::make_unique<CBaseChainParams>("liquidv1", 7041, 8332, 37041);
    } else if (chain == CBaseChainParams::LIQUID1TEST) {
        return std::make_unique<CBaseChainParams>("liquidv1test", 7040, 18332, 37040);  // Use same ports as customparams
    } else if (chain == CBaseChainParams::LIQUIDTESTNET) {
        return std::make_unique<CBaseChainParams>(chain, 7039, 18331, 37039);
    }

    // ELEMENTS:
    return std::make_unique<CBaseChainParams>(chain, 7040, 18332, 37040);
}

void SelectBaseParams(const std::string& chain)
{
    globalChainBaseParams = CreateBaseChainParams(chain);
    gArgs.SelectConfigNetwork(chain);
}
