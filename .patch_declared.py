p = "src/rpc/blockchain.cpp"
s = open(p).read()

# ---- help: say what a pool IS -------------------------------------------------
old = '''                "\\nSEQUENTIA: every block-producing signer on the network, with the weight it commands, who lent\\n"
                "it, what it has committed to paying, and how reliably it has been producing. This is the pool\\n"
                "listing board: read it before delegating with delegatestake, and watch it afterwards.\\n"
                "\\nA signer appears here if it commands any stake weight or has announced a payout policy. A pool\\n"
                "with no policy keeps everything it earns -- that is the default, not a bug, and the listing says\\n"
                "so plainly. A policy change must be announced at least the chain's notice period ahead of\\n"
                "binding, and shows up under `policy_pending` for that whole window, which is the time a\\n"
                "delegator has to leave. Leaving is instant and unilateral (undelegatestake).\\n"'''
new = '''                "\\nSEQUENTIA: the staking pools on the network, with the weight each commands, who lent it, what\\n"
                "it has committed to paying, and how reliably it has been producing. Read it before delegating\\n"
                "with delegatestake, and watch it afterwards.\\n"
                "\\nA POOL IS A SIGNER THAT HAS DECLARED ITSELF ONE, by announcing a payout policy\\n"
                "(announcepayout). That is the only deliberate, on-chain opt-in there is, and it is what\\n"
                "separates an operator soliciting delegations from the many stakers who simply produce blocks\\n"
                "for themselves. Anyone may delegate to any signer -- consensus cannot restrict that and does\\n"
                "not try -- but a signer that has never made a commitment is not running a pool, and listing it\\n"
                "as one would put words in its mouth.\\n"
                "\\nA policy that is announced but has not bound yet still counts as a declaration: that is\\n"
                "exactly what a new pool looks like while it serves its notice period, and it needs to be\\n"
                "findable then.\\n"
                "\\nPass `include_undeclared` to see every signer with weight, each flagged `declared`; pass an\\n"
                "explicit `signer` to read one whether or not it has declared, which is how a wallet describes\\n"
                "the signer its stake is currently lent to.\\n"
                "\\nA policy change must be announced at least the chain's notice period ahead of binding, and\\n"
                "shows up under `policy_pending` for that whole window, which is the time a delegator has to\\n"
                "leave. Leaving is instant and unilateral (undelegatestake).\\n"'''
assert old in s, "help anchor missing"
s = s.replace(old, new, 1)

# ---- params -------------------------------------------------------------------
old = '''                    {"include_delegators", RPCArg::Type::BOOL, RPCArg::Default{false}, "List each pool's individual delegators and their weights."},'''
new = '''                    {"include_delegators", RPCArg::Type::BOOL, RPCArg::Default{false}, "List each pool's individual delegators and their weights."},
                    {"include_undeclared", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also list signers that have declared no payout policy, i.e. stakers producing for themselves rather than pools. Each entry carries `declared` either way."},'''
assert old in s, "param anchor missing"
s = s.replace(old, new, 1)

# ---- result fields -------------------------------------------------------------
old = '''                    {RPCResult::Type::NUM, "window", "blocks the production measurement covers"},'''
new = '''                    {RPCResult::Type::NUM, "window", "blocks the production measurement covers"},
                    {RPCResult::Type::NUM, "declared_pools", "how many signers have declared themselves a pool"},
                    {RPCResult::Type::NUM, "stakers", "how many signers command weight, declared or not; the rest produce for themselves"},'''
assert old in s, "summary anchor missing"
s = s.replace(old, new, 1)

old = '''                            {RPCResult::Type::STR_HEX, "signer", "the block-producing public key"},'''
new = '''                            {RPCResult::Type::STR_HEX, "signer", "the block-producing public key"},
                            {RPCResult::Type::BOOL, "declared", "whether this signer has declared itself a pool by announcing a payout policy"},'''
assert old in s, "signer field anchor missing"
s = s.replace(old, new, 1)

# ---- read the new argument ------------------------------------------------------
old = '''    const bool include_delegators = request.params[2].isNull() ? false : request.params[2].get_bool();'''
new = '''    const bool include_delegators = request.params[2].isNull() ? false : request.params[2].get_bool();
    const bool include_undeclared = request.params[3].isNull() ? false : request.params[3].get_bool();'''
assert old in s, "arg anchor missing"
s = s.replace(old, new, 1)

# ---- inclusion ------------------------------------------------------------------
old = '''    // A signer that has announced a policy but commands nothing yet is still a
    // pool worth listing: that is exactly what a new operator looks like before
    // anyone has delegated to it.
    for (const auto& e : payouts) pools[e.first];'''
new = '''    // A signer that has announced a policy but commands nothing yet is still a
    // pool worth listing: that is exactly what a new operator looks like before
    // anyone has delegated to it.
    for (const auto& e : payouts) pools[e.first];

    // Declaring is announcing a payout policy, in force or still serving its
    // notice. Everything else is a staker producing for itself, however much
    // weight it commands.
    const auto declared = [&](const CPubKey& signer) { return payouts.count(signer) > 0; };'''
assert old in s, "inclusion anchor missing"
s = s.replace(old, new, 1)

# ---- filtering + counts ----------------------------------------------------------
old = '''    UniValue arr(UniValue::VARR);
    for (auto& entry : ordered) {
        const CPubKey& signer = entry.first;
        if (filter && signer != *filter) continue;
        Pool& p = entry.second;
        const uint64_t weight = p.own + p.delegated;

        UniValue o(UniValue::VOBJ);
        o.pushKV("signer", HexStr(signer));'''
new = '''    int64_t declared_count = 0, staker_count = 0;
    for (const auto& entry : ordered) {
        if (declared(entry.first)) ++declared_count;
        if (entry.second.own + entry.second.delegated > 0) ++staker_count;
    }

    UniValue arr(UniValue::VARR);
    for (auto& entry : ordered) {
        const CPubKey& signer = entry.first;
        if (filter && signer != *filter) continue;
        // An explicit signer is always answered, declared or not: a wallet has
        // to be able to describe the signer its stake is lent to, and that
        // signer may well have committed to nothing. Without the filter, only
        // pools, unless the caller asked for the rest.
        if (!filter && !include_undeclared && !declared(signer)) continue;
        Pool& p = entry.second;
        const uint64_t weight = p.own + p.delegated;

        UniValue o(UniValue::VOBJ);
        o.pushKV("signer", HexStr(signer));
        o.pushKV("declared", declared(signer));'''
assert old in s, "loop anchor missing"
s = s.replace(old, new, 1)

# ---- summary counts ---------------------------------------------------------------
old = '''    result.pushKV("window", scanned);
    result.pushKV("pools", arr);'''
new = '''    result.pushKV("window", scanned);
    // Both counts, always, whatever was filtered into `pools`: a board showing no
    // pools still needs to say how many stakers are securing the chain, or an
    // empty list reads as an empty network.
    result.pushKV("declared_pools", declared_count);
    result.pushKV("stakers", staker_count);
    result.pushKV("pools", arr);'''
assert old in s, "result anchor missing"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("listpools: a pool is now an opt-in")
