// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/btcpayment.h>

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <streams.h>
#include <version.h>

namespace BtcPayment {

namespace {

uint32_t ReadLE32At(const std::vector<unsigned char>& v, size_t pos) { return ReadLE32(v.data() + pos); }

uint256 ReadHashAt(const std::vector<unsigned char>& v, size_t pos)
{
    uint256 h;
    std::copy(v.begin() + pos, v.begin() + pos + 32, h.begin());
    return h;
}

/** Hash `leaf` up `branch` by `index`. Fails on a non-canonical index: a 1 bit
 *  at a level where the sibling is the node itself (the duplicated last node of
 *  an odd level can only be the left one). */
bool HashUp(uint256 leaf, uint32_t index, const std::vector<uint256>& branch, uint256& root)
{
    uint256 node = leaf;
    for (const uint256& sib : branch) {
        const bool right = index & 1;
        if (right && sib == node) return false;
        node = right ? Hash(sib, node) : Hash(node, sib);
        index >>= 1;
    }
    if (index != 0) return false;
    root = node;
    return true;
}

} // namespace

bool ParsePaymentRecord(const std::vector<unsigned char>& item, Proof& out, std::string& reason)
{
    // 1. Exact decoding within limits.
    if (item.size() < PAYMENT_FIXED_SIZE || item[0] != RECORD_PAYMENT) {
        reason = "bad-btcpay-encoding";
        return false;
    }
    size_t pos = 1;
    out.height = ReadLE32At(item, pos); pos += 4;
    out.coin_anchor = ReadLE32At(item, pos); pos += 4;
    try {
        CDataStream hs(std::vector<unsigned char>(item.begin() + pos, item.begin() + pos + 80), SER_NETWORK, PROTOCOL_VERSION);
        hs >> out.header;
    } catch (const std::exception&) {
        reason = "bad-btcpay-encoding";
        return false;
    }
    pos += 80;
    const uint256 coinbase_sha = ReadHashAt(item, pos); pos += 32;
    const unsigned int depth = item[pos]; pos += 1;
    const uint32_t index = ReadLE32At(item, pos); pos += 4;
    if (depth < 1 || depth > MAX_DEPTH) {
        reason = "bad-btcpay-encoding";
        return false;
    }
    if (item.size() < pos + 64 * depth) {
        reason = "bad-btcpay-encoding";
        return false;
    }
    std::vector<uint256> payment_branch, coinbase_branch;
    for (unsigned int i = 0; i < depth; ++i) { payment_branch.push_back(ReadHashAt(item, pos)); pos += 32; }
    for (unsigned int i = 0; i < depth; ++i) { coinbase_branch.push_back(ReadHashAt(item, pos)); pos += 32; }
    // The headers linking the proven block forward, each committing to the one
    // before it, so they are the proven block's real descendants.
    if (item.size() < pos + 2) { reason = "bad-btcpay-encoding"; return false; }
    out.links = ReadLE16(item.data() + pos); pos += 2;
    if (out.links > MAX_LINKS || item.size() < pos + 80 * (size_t)out.links) {
        reason = "bad-btcpay-encoding";
        return false;
    }
    out.block_hash = out.header.GetHash();
    uint256 prev = out.block_hash;
    for (uint32_t i = 0; i < out.links; ++i) {
        Sidechain::Bitcoin::CBlockHeader link;
        CDataStream ls(std::vector<unsigned char>(item.begin() + pos, item.begin() + pos + 80), SER_NETWORK, PROTOCOL_VERSION);
        ls >> link;
        pos += 80;
        if (link.hashPrevBlock != prev) { reason = "bad-btcpay-links"; return false; }
        prev = link.GetHash();
    }
    out.link_end = prev;
    const size_t tx_size = item.size() - pos;
    if (tx_size == 0 || tx_size > MAX_PAYMENT_TX_SIZE) {
        reason = "bad-btcpay-encoding";
        return false;
    }
    // 4. The payment: parses without witness, consuming every byte; not a
    // coinbase; not 64 bytes long.
    if (tx_size == 64) {
        reason = "bad-btcpay-tx-64-bytes";
        return false;
    }
    try {
        CDataStream ts(std::vector<unsigned char>(item.begin() + pos, item.end()), SER_NETWORK,
                       PROTOCOL_VERSION | Sidechain::Bitcoin::SERIALIZE_TRANSACTION_NO_WITNESS);
        out.tx = Sidechain::Bitcoin::MakeTransactionRef(Sidechain::Bitcoin::CMutableTransaction(deserialize, ts));
        if (!ts.empty()) {
            reason = "bad-btcpay-tx-encoding";
            return false;
        }
    } catch (const std::exception&) {
        reason = "bad-btcpay-tx-encoding";
        return false;
    }
    if (out.tx->IsCoinBase()) {
        reason = "bad-btcpay-tx-coinbase";
        return false;
    }
    // 2 and 3. Both branches reach the header's merkle root; the payment at a
    // canonical index above 0, the coinbase at index 0.
    if (index == 0) {
        reason = "bad-btcpay-index";
        return false;
    }
    uint256 root_payment, root_coinbase;
    if (!HashUp(out.tx->GetHash(), index, payment_branch, root_payment)) {
        reason = "bad-btcpay-index";
        return false;
    }
    uint256 coinbase_leaf;
    CSHA256().Write(coinbase_sha.begin(), 32).Finalize(coinbase_leaf.begin());
    if (!HashUp(coinbase_leaf, 0, coinbase_branch, root_coinbase)) {
        reason = "bad-btcpay-index";
        return false;
    }
    if (root_payment != out.header.hashMerkleRoot || root_coinbase != out.header.hashMerkleRoot) {
        reason = "bad-btcpay-merkle";
        return false;
    }
    out.spk_hashes.clear();
    for (const auto& o : out.tx->vout) {
        uint256 h;
        CSHA256().Write(o.scriptPubKey.data(), o.scriptPubKey.size()).Finalize(h.begin());
        out.spk_hashes.push_back(h);
    }
    return true;
}

bool ParseRecords(const std::vector<std::vector<unsigned char>>& slot, Records& out, std::string& reason)
{
    out = Records{};
    // Records of a known type must be valid and appear at most once. Items of
    // any other type are ignored by consensus, so a later record type can be
    // added as a tightening; relay policy refuses them (IsStandardBtcPaymentSlot).
    for (const auto& item : slot) {
        if (item.empty()) continue;
        if (item[0] == RECORD_PAYMENT) {
            if (out.proof) { reason = "bad-btcpay-duplicate"; return false; }
            Proof p;
            if (!ParsePaymentRecord(item, p, reason)) return false;
            out.proof = std::move(p);
        } else if (item[0] == RECORD_FLOOR) {
            if (out.floor) { reason = "bad-anchorlock-duplicate"; return false; }
            if (item.size() != 5) { reason = "bad-anchorlock-encoding"; return false; }
            out.floor = ReadLE32At(item, 1);
        }
    }
    return true;
}

std::vector<unsigned char> BuildPaymentRecord(uint32_t height, uint32_t coin_anchor,
                                              const Sidechain::Bitcoin::CBlockHeader& header,
                                              const uint256& coinbase_sha256, uint32_t index,
                                              const std::vector<uint256>& payment_branch,
                                              const std::vector<uint256>& coinbase_branch,
                                              const std::vector<Sidechain::Bitcoin::CBlockHeader>& links,
                                              const std::vector<unsigned char>& payment_tx_stripped)
{
    std::vector<unsigned char> v;
    v.push_back(RECORD_PAYMENT);
    unsigned char b4[4];
    WriteLE32(b4, height); v.insert(v.end(), b4, b4 + 4);
    WriteLE32(b4, coin_anchor); v.insert(v.end(), b4, b4 + 4);
    CDataStream hs(SER_NETWORK, PROTOCOL_VERSION);
    hs << header;
    const auto* hp = reinterpret_cast<const unsigned char*>(hs.data());
    v.insert(v.end(), hp, hp + hs.size());
    v.insert(v.end(), coinbase_sha256.begin(), coinbase_sha256.end());
    v.push_back((unsigned char)payment_branch.size());
    WriteLE32(b4, index); v.insert(v.end(), b4, b4 + 4);
    for (const auto& h : payment_branch) v.insert(v.end(), h.begin(), h.end());
    for (const auto& h : coinbase_branch) v.insert(v.end(), h.begin(), h.end());
    unsigned char b2[2];
    WriteLE16(b2, (uint16_t)links.size()); v.insert(v.end(), b2, b2 + 2);
    for (const auto& l : links) {
        CDataStream ls(SER_NETWORK, PROTOCOL_VERSION);
        ls << l;
        const auto* lp = reinterpret_cast<const unsigned char*>(ls.data());
        v.insert(v.end(), lp, lp + ls.size());
    }
    v.insert(v.end(), payment_tx_stripped.begin(), payment_tx_stripped.end());
    return v;
}

std::vector<uint256> MerkleBranch(const std::vector<uint256>& leaves, uint32_t index)
{
    std::vector<uint256> branch;
    std::vector<uint256> level = leaves;
    while (level.size() > 1) {
        if (level.size() & 1) level.push_back(level.back());
        branch.push_back(level[index ^ 1]);
        std::vector<uint256> next;
        for (size_t i = 0; i < level.size(); i += 2) next.push_back(Hash(level[i], level[i + 1]));
        level.swap(next);
        index >>= 1;
    }
    return branch;
}

} // namespace BtcPayment
