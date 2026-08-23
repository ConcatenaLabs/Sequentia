// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <seqdexclient.h>

#include <support/events.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace {

struct HTTPReply {
    HTTPReply() = default;
    int status{0};
    int error{-1};
    std::string body;
};

void http_request_done(struct evhttp_request* req, void* ctx)
{
    HTTPReply* reply = static_cast<HTTPReply*>(ctx);
    if (req == nullptr) {
        reply->status = 0;
        return;
    }
    reply->status = evhttp_request_get_response_code(req);
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (buf) {
        size_t size = evbuffer_get_length(buf);
        const char* data = (const char*)evbuffer_pullup(buf, size);
        if (data) reply->body = std::string(data, size);
        evbuffer_drain(buf, size);
    }
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void http_error_cb(enum evhttp_request_error err, void* ctx)
{
    static_cast<HTTPReply*>(ctx)->error = err;
}
#endif

/** Split "http://host:port/prefix" into its parts. Only plain http: this node
 *  links no TLS, and pretending otherwise would fail at connect time with a
 *  mystery rather than at parse time with a reason. */
struct ParsedUrl {
    std::string host;
    uint16_t port{80};
    std::string prefix;   //!< path prefix, no trailing slash
};

std::optional<ParsedUrl> ParseRelayUrl(const std::string& url)
{
    static const std::string kScheme = "http://";
    if (url.rfind(kScheme, 0) != 0) return std::nullopt;
    std::string rest = url.substr(kScheme.size());
    std::string hostport = rest;
    std::string prefix;
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        hostport = rest.substr(0, slash);
        prefix = rest.substr(slash);
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    }
    ParsedUrl out;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        const std::string p = hostport.substr(colon + 1);
        if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        const int n = atoi(p.c_str());
        if (n <= 0 || n > 65535) return std::nullopt;
        out.port = (uint16_t)n;
        out.host = hostport.substr(0, colon);
    } else {
        out.host = hostport;
    }
    if (out.host.empty()) return std::nullopt;
    out.prefix = prefix;
    return out;
}

//! An offer's asset ids are DISPLAY hex, the reversed form everything
//! user-facing prints.
//! The book names native Bitcoin "BTC", not an asset id: on the parent chain it
//! has none.
bool IsNativeBtcField(const UniValue& v)
{
    if (!v.isStr()) return false;
    const std::string s = v.get_str();
    return s == "BTC" || s == "btc";
}

CAsset AssetFromHexField(const UniValue& v)
{
    if (!v.isStr()) return CAsset();
    const std::string s = v.get_str();
    if (s.size() != 64 || !IsHex(s)) return CAsset();
    return CAsset(uint256S(s));
}

//! A COVENANT's asset ids are the other way round: they come straight from the
//! relay's CovenantTerms and are INTERNAL byte order, because that is the order
//! the leaf script bakes them in. Parsing these the display way silently
//! reverses them, every covenant then fails its own asset check, and the whole
//! passive book looks empty -- which is indistinguishable from "no liquidity"
//! and is the reason this is spelled out rather than inferred.
CAsset AssetFromRawHexField(const UniValue& v)
{
    if (!v.isStr()) return CAsset();
    const std::string s = v.get_str();
    if (s.size() != 64 || !IsHex(s)) return CAsset();
    const std::vector<unsigned char> raw = ParseHex(s);
    return CAsset(uint256(raw));
}

//! Amounts cross the wire as decimal STRINGS (jstype = JS_STRING), because they
//! do not survive a double. Accept a number too, since some encoders emit one.
CAmount AmountFromField(const UniValue& v)
{
    if (v.isStr()) {
        const std::string s = v.get_str();
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return 0;
        errno = 0;
        const long long n = strtoll(s.c_str(), nullptr, 10);
        if (errno != 0 || n < 0) return 0;
        return (CAmount)n;
    }
    if (v.isNum()) {
        const int64_t n = v.get_int64();
        return n > 0 ? (CAmount)n : 0;
    }
    return 0;
}

const UniValue& Field(const UniValue& o, const std::string& a, const std::string& b)
{
    static const UniValue null_value;
    if (o.exists(a)) return o[a];
    if (o.exists(b)) return o[b];
    return null_value;
}

std::vector<unsigned char> BytesFromField(const UniValue& v)
{
    if (!v.isStr()) return {};
    const std::string s = v.get_str();
    // The relay's protojson emits proto `bytes` as base64; a few clients post
    // hex. Take whichever parses to something of a sane length.
    if (s.size() == 64 && IsHex(s)) return ParseHex(s);
    bool invalid = false;
    const std::string decoded = DecodeBase64(s, &invalid);
    if (invalid) return {};
    return std::vector<unsigned char>(decoded.begin(), decoded.end());
}

std::optional<SeqobCovenant> ParseCovenant(const UniValue& c)
{
    if (!c.isObject()) return std::nullopt;
    SeqobCovenant cov;
    const UniValue& txid = Field(c, "covenant_txid", "covenantTxid");
    if (!txid.isStr() || txid.get_str().size() != 64 || !IsHex(txid.get_str())) return std::nullopt;
    cov.txid = uint256S(txid.get_str());
    const UniValue& vout = Field(c, "covenant_vout", "covenantVout");
    cov.vout = vout.isNum() ? (uint32_t)vout.get_int64() : 0;
    cov.asset_a = AssetFromRawHexField(Field(c, "asset_a", "assetA"));
    cov.asset_b = AssetFromRawHexField(Field(c, "asset_b", "assetB"));
    if (cov.asset_a.IsNull() || cov.asset_b.IsNull()) return std::nullopt;
    cov.rate_num = AmountFromField(Field(c, "rate_num", "rateNum"));
    cov.rate_den = AmountFromField(Field(c, "rate_den", "rateDen"));
    cov.min_lot = AmountFromField(Field(c, "min_lot", "minLot"));
    if (cov.rate_num <= 0 || cov.rate_den <= 0 || cov.min_lot <= 0) return std::nullopt;
    cov.maker_prog = BytesFromField(Field(c, "maker_prog", "makerProg"));
    cov.maker_x = BytesFromField(Field(c, "maker_x", "makerX"));
    cov.internal_key = BytesFromField(Field(c, "internal_key", "internalKey"));
    if (cov.maker_prog.size() != 32 || cov.internal_key.size() != 32) return std::nullopt;
    const UniValue& mp = Field(c, "merkle_path", "merklePath");
    if (mp.isArray()) {
        for (size_t i = 0; i < mp.size(); ++i) {
            std::vector<unsigned char> h = BytesFromField(mp[i]);
            if (h.size() != 32) return std::nullopt;
            cov.merkle_path.push_back(std::move(h));
        }
    }
    const UniValue& exp = Field(c, "expiry_locktime", "expiryLocktime");
    cov.expiry_locktime = exp.isNum() ? exp.get_int64() : AmountFromField(exp);
    return cov;
}

std::optional<SeqobOffer> ParseOffer(const UniValue& o)
{
    if (!o.isObject()) return std::nullopt;
    SeqobOffer off;
    const UniValue& id = Field(o, "offer_id", "offerId");
    if (!id.isStr()) return std::nullopt;
    off.offer_id = id.get_str();
    const UniValue& mk = Field(o, "maker_pubkey", "makerPubkey");
    off.maker_pubkey = mk.isStr() ? mk.get_str() : "";
    const UniValue& oa = Field(o, "offer_asset", "offerAsset");
    const UniValue& wa = Field(o, "want_asset", "wantAsset");
    off.offer_is_btc = IsNativeBtcField(oa);
    off.want_is_btc = IsNativeBtcField(wa);
    off.offer_asset = off.offer_is_btc ? CAsset() : AssetFromHexField(oa);
    off.want_asset = off.want_is_btc ? CAsset() : AssetFromHexField(wa);
    off.offer_amount = AmountFromField(Field(o, "offer_amount", "offerAmount"));
    off.want_amount = AmountFromField(Field(o, "want_amount", "wantAmount"));
    // Exactly one side may be BTC: an offer with neither asset resolved is
    // noise, and one with both is not a trade.
    if (off.offer_is_btc && off.want_is_btc) return std::nullopt;
    if (!off.offer_is_btc && off.offer_asset.IsNull()) return std::nullopt;
    if (!off.want_is_btc && off.want_asset.IsNull()) return std::nullopt;
    if (off.offer_amount <= 0 || off.want_amount <= 0) return std::nullopt;
    const UniValue& ap = Field(o, "allow_partial", "allowPartial");
    off.allow_partial = ap.isBool() ? ap.get_bool() : false;
    const UniValue& exp = Field(o, "expires_at_unix", "expiresAtUnix");
    off.expires_at_unix = exp.isNum() ? exp.get_int64() : AmountFromField(exp);
    off.covenant = ParseCovenant(Field(o, "covenant", "Covenant"));
    return off;
}

} // namespace

std::vector<SeqobOffer> SeqobParseBook(const UniValue& j)
{
    std::vector<SeqobOffer> out;
    const UniValue& offers = Field(j, "offers", "Offers");
    if (!offers.isArray()) return out;
    for (size_t i = 0; i < offers.size(); ++i) {
        auto off = ParseOffer(offers[i]);
        if (off) out.push_back(std::move(*off));
    }
    return out;
}

std::string SeqdexRelayUrl()
{
    return gArgs.GetArg("-seqoburl", "");
}

bool SeqdexRelayEndpoint(std::string& host, uint16_t& port, std::string& path_prefix)
{
    const auto url = ParseRelayUrl(SeqdexRelayUrl());
    if (!url) return false;
    host = url->host;
    port = url->port;
    path_prefix = url->prefix;
    return true;
}

UniValue SeqdexHttpJson(const std::string& path, const std::string& method, const UniValue& body)
{
    const std::string base = SeqdexRelayUrl();
    if (base.empty()) {
        throw std::runtime_error("no SeqDEX relay configured: set -seqoburl=http://host:port");
    }
    const auto url = ParseRelayUrl(base);
    if (!url) {
        throw std::runtime_error(strprintf(
            "-seqoburl must be a plain http:// URL (this node links no TLS): %s", base));
    }

    raii_event_base ev_base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(ev_base.get(), url->host, url->port);
    evhttp_connection_set_timeout(evcon.get(), gArgs.GetIntArg("-seqobtimeout", 30));

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    if (req == nullptr) throw std::runtime_error("create http request failed");
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), http_error_cb);
#endif

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    assert(output_headers);
    evhttp_add_header(output_headers, "Host", url->host.c_str());
    evhttp_add_header(output_headers, "Connection", "close");
    evhttp_add_header(output_headers, "Accept", "application/json");

    const bool post = (method == "POST");
    if (post) {
        evhttp_add_header(output_headers, "Content-Type", "application/json");
        const std::string payload = body.write();
        struct evbuffer* output_buffer = evhttp_request_get_output_buffer(req.get());
        assert(output_buffer);
        evbuffer_add(output_buffer, payload.data(), payload.size());
    }

    const std::string full_path = url->prefix + path;
    const int r = evhttp_make_request(evcon.get(), req.get(),
                                      post ? EVHTTP_REQ_POST : EVHTTP_REQ_GET, full_path.c_str());
    req.release();   // ownership moved to evcon
    if (r != 0) throw std::runtime_error("send http request failed");

    event_base_dispatch(ev_base.get());

    if (response.status == 0) {
        throw std::runtime_error(strprintf("could not reach the SeqDEX relay at %s", base));
    }
    if (response.status >= 400) {
        throw std::runtime_error(strprintf("SeqDEX relay returned HTTP %d for %s", response.status, full_path));
    }
    UniValue out;
    if (!out.read(response.body)) {
        throw std::runtime_error("SeqDEX relay returned a body that is not JSON");
    }
    return out;
}

std::vector<SeqobOffer> SeqobFetchBook(const CAsset& base, const CAsset& quote)
{
    std::vector<SeqobOffer> out;
    std::set<std::string> seen;

    // BOTH orientations. The relay keys markets by exact base/quote order, so an
    // offer selling A for B rests under whichever heading its maker posted it,
    // and asking for one silently halves the book.
    const std::vector<std::pair<CAsset, CAsset>> pairs{{base, quote}, {quote, base}};
    for (const auto& p : pairs) {
        UniValue j;
        try {
            j = SeqdexHttpJson(strprintf("/v1/market/%s/%s/orderbook",
                                         p.first.GetHex(), p.second.GetHex()), "GET");
        } catch (const std::exception&) {
            continue;   // one orientation missing is normal; both failing is caught by the caller
        }
        for (SeqobOffer& off : SeqobParseBook(j)) {
            const std::string key = off.maker_pubkey + ":" + off.offer_id;
            if (!seen.insert(key).second) continue;   // a maker that seeded both orientations
            out.push_back(std::move(off));
        }
    }
    return out;
}

std::vector<SeqobOffer> SeqobFetchBtcBids(const CAsset& asset)
{
    std::vector<SeqobOffer> out;
    std::set<std::string> seen;
    const std::vector<std::pair<std::string, std::string>> pairs{
        {asset.GetHex(), "BTC"}, {"BTC", asset.GetHex()}};
    for (const auto& p : pairs) {
        UniValue j;
        try {
            j = SeqdexHttpJson(strprintf("/v1/market/%s/%s/orderbook", p.first, p.second), "GET");
        } catch (const std::exception&) {
            continue;
        }
        for (SeqobOffer& off : SeqobParseBook(j)) {
            // A reverse offer is simply one that GIVES Bitcoin and wants this
            // asset. A forward offer (wants BTC) rests in the same books and is
            // not what a seller takes.
            if (!off.offer_is_btc) continue;
            if (off.want_is_btc || !(off.want_asset == asset)) continue;
            const std::string key = off.maker_pubkey + ":" + off.offer_id;
            if (!seen.insert(key).second) continue;
            out.push_back(std::move(off));
        }
    }
    // Most satoshis per unit of the asset first: that is what "best" means to
    // whoever is selling it.
    std::sort(out.begin(), out.end(), [](const SeqobOffer& a, const SeqobOffer& b) {
        const double ra = a.want_amount > 0 ? (double)a.offer_amount / (double)a.want_amount : 0.0;
        const double rb = b.want_amount > 0 ? (double)b.offer_amount / (double)b.want_amount : 0.0;
        return ra > rb;
    });
    return out;
}

CAmount SeqobCovenantPrice(CAmount filled, int64_t rate_num, int64_t rate_den)
{
    if (filled <= 0 || rate_num <= 0 || rate_den <= 0) return 0;
    // ceil(filled*num/den), the covenant's own arithmetic. Guard the multiply:
    // a rate that overflows is a rate this node will not trade against, rather
    // than one it silently misprices.
    if (filled > std::numeric_limits<int64_t>::max() / rate_num) return 0;
    const int64_t prod = filled * rate_num;
    if (prod > std::numeric_limits<int64_t>::max() - (rate_den - 1)) return 0;
    return (prod + rate_den - 1) / rate_den;
}

std::optional<SeqobWalk> SeqobWalkBook(const std::vector<SeqobOffer>& book,
                                       const CAsset& sell, const CAsset& want,
                                       CAmount atoms)
{
    if (atoms <= 0) return std::nullopt;

    // Crossable: the maker GIVES what we want and WANTS what we are selling.
    // Only covenant-backed offers, because those are the ones this node can
    // take on its own; an interactive offer needs its maker online to co-sign,
    // which is a conversation the node cannot have.
    std::vector<SeqobOffer> asks;
    for (const SeqobOffer& o : book) {
        if (!(o.offer_asset == want) || !(o.want_asset == sell)) continue;
        if (!o.covenant) continue;
        if (!(o.covenant->asset_a == want) || !(o.covenant->asset_b == sell)) continue;
        asks.push_back(o);
    }
    if (asks.empty()) return std::nullopt;

    // Best first = most `want` per unit of `sell`.
    std::sort(asks.begin(), asks.end(), [](const SeqobOffer& a, const SeqobOffer& b) {
        const double ra = a.want_amount > 0 ? (double)a.offer_amount / (double)a.want_amount : 0.0;
        const double rb = b.want_amount > 0 ? (double)b.offer_amount / (double)b.want_amount : 0.0;
        return ra > rb;
    });

    const double best_rate = asks[0].want_amount > 0
        ? (double)asks[0].offer_amount / (double)asks[0].want_amount : 0.0;
    if (best_rate <= 0) return std::nullopt;

    SeqobWalk walk;
    CAmount left = atoms;
    for (const SeqobOffer& o : asks) {
        if (left <= 0) break;
        // What this offer will take of ours, capped by its own size.
        const CAmount takes = std::min(left, o.want_amount);
        if (takes <= 0) continue;
        // ...and what it pays for that, at ITS rate, floored (the maker's favour,
        // which is also what the covenant enforces).
        const CAmount pays = (CAmount)(((__int128)takes * o.offer_amount) / o.want_amount);
        if (pays <= 0) continue;
        // A covenant will not accept a fill below its minimum lot, and the
        // remainder it keeps must clear it too -- a fill that would strand the
        // rest below min_lot is one the script rejects, so do not plan it.
        if (o.covenant->min_lot > 0) {
            if (pays < o.covenant->min_lot) continue;
            const CAmount remainder = o.offer_amount - pays;
            if (remainder != 0 && remainder < o.covenant->min_lot) continue;
        }
        SeqobWalkLeg leg;
        leg.offer = o;
        leg.pay = takes;
        leg.receive = pays;
        walk.legs.push_back(std::move(leg));
        walk.receives += pays;
        left -= takes;
    }
    if (left > 0 || walk.legs.empty()) return std::nullopt;   // not enough depth to fill it all

    walk.reference = (CAmount)(((__int128)atoms * asks[0].offer_amount) / asks[0].want_amount);
    return walk;
}
