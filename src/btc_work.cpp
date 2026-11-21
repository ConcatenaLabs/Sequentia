// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <btc_work.h>

#include <arith_uint256.h>

#include <algorithm>

namespace BtcWork {
namespace {

constexpr uint32_t INTERVAL = 2016;
constexpr int64_t TIMESPAN = 14 * 24 * 60 * 60;
constexpr int64_t SPACING = 10 * 60;

arith_uint256 PowLimit(Net net)
{
    return UintToArith256(uint256S(net == Net::REGTEST
        ? "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        : "00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
}

//! The target of `bits`, or nullopt when it is not a valid one for `net`.
std::optional<arith_uint256> TargetOf(Net net, uint32_t bits)
{
    bool negative = false, overflow = false;
    arith_uint256 target;
    target.SetCompact(bits, &negative, &overflow);
    if (negative || overflow || target == 0 || target > PowLimit(net)) return std::nullopt;
    return target;
}

} // namespace

std::optional<uint32_t> RequiredBits(Net net, const Ancestor& parent, uint32_t time, bool real, const Lookup& at)
{
    if (net == Net::REGTEST) return parent.bits;
    const arith_uint256 limit = PowLimit(net);
    const uint32_t limit_bits = limit.GetCompact();
    if ((parent.height + 1) % INTERVAL != 0) {
        if (net == Net::MAIN) return parent.bits;
        if (!real && (int64_t)time > (int64_t)parent.time + 2 * SPACING) return limit_bits;
        // The last block of this period that did not use the exception.
        Ancestor p = parent;
        while (p.height % INTERVAL != 0 && p.bits == limit_bits) {
            const std::optional<Ancestor> up = at(p.prev);
            if (!up || up->height + 1 != p.height) return std::nullopt;
            p = *up;
        }
        return p.bits;
    }
    // A retarget, from the period that ends at `parent`.
    Ancestor first = parent;
    for (uint32_t i = 0; i < INTERVAL - 1; ++i) {
        const std::optional<Ancestor> up = at(first.prev);
        if (!up || up->height + 1 != first.height) return std::nullopt;
        first = *up;
    }
    int64_t span = (int64_t)parent.time - (int64_t)first.time;
    span = std::clamp<int64_t>(span, TIMESPAN / 4, TIMESPAN * 4);
    arith_uint256 target;
    // BIP94: testnet4 scales the first block's target of the period, so a run
    // of minimum-difficulty blocks at its end cannot reset the difficulty.
    target.SetCompact(net == Net::TESTNET4 ? first.bits : parent.bits);
    target *= (uint32_t)span;
    target /= (uint32_t)TIMESPAN;
    if (target > limit) target = limit;
    return target.GetCompact();
}

Verdict Judge(Net net, const Sidechain::Bitcoin::CBlockHeader& header, uint32_t height, const Lookup& at)
{
    Verdict v;
    const std::optional<Ancestor> parent = at(header.hashPrevBlock);
    if (!parent) { v.state = State::UNANSWERED; v.why = "btc-header-parent-unread"; return v; }
    if (parent->height + 1 != height) { v.why = "btc-header-height"; return v; }
    const std::optional<uint32_t> required = RequiredBits(net, *parent, header.nTime, /*real=*/false, at);
    const std::optional<uint32_t> real = RequiredBits(net, *parent, header.nTime, /*real=*/true, at);
    if (!required || !real) { v.state = State::UNANSWERED; v.why = "btc-header-ancestor-unread"; return v; }
    if (header.nBits != *required) { v.why = "btc-header-bits"; return v; }
    const std::optional<arith_uint256> target = TargetOf(net, header.nBits);
    if (!target) { v.why = "btc-header-target"; return v; }
    const arith_uint256 hash = UintToArith256(header.GetHash());
    if (hash > *target) { v.why = "btc-header-work"; return v; }
    const std::optional<arith_uint256> real_target = TargetOf(net, *real);
    v.real = real_target && hash <= *real_target;
    v.state = State::OK;
    return v;
}

} // namespace BtcWork
