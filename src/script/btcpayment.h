// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_BTCPAYMENT_H
#define BITCOIN_SCRIPT_BTCPAYMENT_H

// SEQUENTIA: records a spender places in the per-input witness slot of a
// non-peg-in input: a proof of a bitcoin payment, and an anchor floor.
// doc/sequentia/bitcoin-payment-proof.md specifies both. Everything here is a
// pure function of the record's bytes; what depends on the chain (the anchor
// chain, the coin's anchor) is checked by validation.

#include <primitives/bitcoin/block.h>
#include <primitives/bitcoin/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <optional>
#include <string>
#include <vector>

namespace BtcPayment {

static constexpr unsigned char RECORD_PAYMENT = 0x01;
static constexpr unsigned char RECORD_FLOOR = 0x02;
//! Branch length bound: 2^24 leaves is far above any Bitcoin block.
static constexpr unsigned int MAX_DEPTH = 24;
//! Bitcoin's standardness ceiling for a relayed transaction, stripped bytes.
static constexpr size_t MAX_PAYMENT_TX_SIZE = 100000;
//! Headers that may link the proven block forward to a recorded anchor.
static constexpr size_t MAX_LINKS = 2016;
//! type, height, coin anchor, header, SHA256(coinbase), depth, index.
static constexpr size_t PAYMENT_FIXED_SIZE = 1 + 4 + 4 + 80 + 32 + 1 + 4;

struct Proof {
    uint32_t height{0};       //!< declared Bitcoin height of the proven block (checked against the chain)
    uint32_t coin_anchor{0};  //!< declared anchor height of the spent coin's block (checked against the chain)
    Sidechain::Bitcoin::CBlockHeader header;
    uint256 block_hash;
    //! Number of headers linking the proven block forward, and the hash they end
    //! at (the proven block's own hash when there are none). That hash must be an
    //! anchor recorded by the claim's block or one of its ancestors.
    uint32_t links{0};
    uint256 link_end;
    Sidechain::Bitcoin::CTransactionRef tx;
    //! SHA-256 of each output script of the payment, computed once.
    std::vector<uint256> spk_hashes;
};

struct Records {
    std::optional<Proof> proof;
    std::optional<uint32_t> floor;
    bool Empty() const { return !proof && !floor; }
};

/** Decode and check one payment record (checks 1 to 4 of the specification).
 *  On failure `reason` names the check. */
bool ParsePaymentRecord(const std::vector<unsigned char>& item, Proof& out, std::string& reason);

/** Decode the slot of a non-peg-in input. An empty slot is valid and has no
 *  records. Anything other than records 0x01 then 0x02, each at most once and
 *  each valid, is invalid. */
bool ParseRecords(const std::vector<std::vector<unsigned char>>& slot, Records& out, std::string& reason);

/** Build a payment record (for tests and the node's own re-attachment). */
std::vector<unsigned char> BuildPaymentRecord(uint32_t height, uint32_t coin_anchor,
                                              const Sidechain::Bitcoin::CBlockHeader& header,
                                              const uint256& coinbase_sha256, uint32_t index,
                                              const std::vector<uint256>& payment_branch,
                                              const std::vector<uint256>& coinbase_branch,
                                              const std::vector<Sidechain::Bitcoin::CBlockHeader>& links,
                                              const std::vector<unsigned char>& payment_tx_stripped);

/** The branch and canonical index of leaf `index` among `leaves` (Bitcoin's
 *  merkle tree, last node duplicated on odd levels). */
std::vector<uint256> MerkleBranch(const std::vector<uint256>& leaves, uint32_t index);

} // namespace BtcPayment

#endif // BITCOIN_SCRIPT_BTCPAYMENT_H
