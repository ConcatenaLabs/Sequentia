// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BTC_WORK_H
#define BITCOIN_BTC_WORK_H

// SEQUENTIA: Bitcoin's proof-of-work rule for a header this chain carries
// without its parent network's node having seen it (a stale block named by a
// lost block, doc/sequentia/03-bitcoin-anchoring.md, "Records and bindings").
// A pure function of the header and of the ancestors `Lookup` returns.

#include <primitives/bitcoin/block.h>
#include <uint256.h>

#include <functional>
#include <optional>
#include <string>

namespace BtcWork {

//! The parent network whose difficulty rule applies.
enum class Net { MAIN, TESTNET4, REGTEST };

//! What the rule reads of one ancestor.
struct Ancestor {
    uint256 hash;
    uint256 prev;
    uint32_t height{0};
    uint32_t time{0};
    uint32_t bits{0};
};
//! The ancestor with this block hash, or nullopt when it cannot be read.
using Lookup = std::function<std::optional<Ancestor>(const uint256&)>;

enum class State { OK, BAD, UNANSWERED };

struct Verdict {
    State state{State::BAD};
    //! The hash also meets the network's real difficulty at this height: the
    //! required target with testnet4's 20-minute exception set aside.
    bool real{false};
    std::string why;
};

/** The nBits Bitcoin requires of the child of `parent` with timestamp `time`.
 *  With `real`, testnet4's exception for a block more than 20 minutes after its
 *  parent is set aside. nullopt when an ancestor cannot be read. */
std::optional<uint32_t> RequiredBits(Net net, const Ancestor& parent, uint32_t time, bool real, const Lookup& at);

/** Judge `header` as the block at `height`, whose parent `at` must return.
 *  OK when its nBits is what Bitcoin requires there (across a 2016-block
 *  boundary too) and its hash meets that target. UNANSWERED when an ancestor
 *  the rule reads cannot be read: never a verdict on the header. */
Verdict Judge(Net net, const Sidechain::Bitcoin::CBlockHeader& header, uint32_t height, const Lookup& at);

} // namespace BtcWork

#endif // BITCOIN_BTC_WORK_H
