// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assetcontract.h>

#include <crypto/sha256.h>
#include <tinyformat.h>
#include <univalue.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace AssetContract {

namespace {

/** Escape a string the way JavaScript's JSON.stringify does.
 *
 * Only the quote, the backslash and C0 control characters are escaped; DEL and
 * everything above it pass through as raw UTF-8. This matters: UniValue's own
 * writer additionally escapes DEL, which would hash differently from the
 * registry's Node implementation. Validate() rejects control characters and
 * DEL outright so the two can never disagree in practice, but the writer here is
 * the one that defines the bytes we hash, so it follows JSON.stringify exactly.
 */
void AppendEscapedString(const std::string& s, std::string& out)
{
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20) {
                out += strprintf("\\u%04x", static_cast<unsigned int>(c));
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void AppendCanonical(const UniValue& v, std::string& out)
{
    switch (v.getType()) {
    case UniValue::VOBJ: {
        // Sort by the keys' raw bytes, matching Array.prototype.sort() on the
        // registry side (which compares UTF-16 code units -- the same order for
        // the ASCII keys a contract is allowed to carry).
        std::vector<std::string> keys = v.getKeys();
        std::sort(keys.begin(), keys.end());
        out += '{';
        bool first = true;
        for (const std::string& k : keys) {
            if (!first) out += ',';
            first = false;
            AppendEscapedString(k, out);
            out += ':';
            AppendCanonical(v[k], out);
        }
        out += '}';
        break;
    }
    case UniValue::VARR: {
        out += '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            AppendCanonical(v[i], out);
        }
        out += ']';
        break;
    }
    case UniValue::VSTR:
        AppendEscapedString(v.getValStr(), out);
        break;
    case UniValue::VNUM:
        // UniValue keeps the number's literal text, which for the integers a
        // contract carries is what JSON.stringify would emit.
        out += v.getValStr();
        break;
    case UniValue::VBOOL:
        out += v.get_bool() ? "true" : "false";
        break;
    case UniValue::VNULL:
        out += "null";
        break;
    }
}

bool IsLowerHex(const std::string& s, size_t len)
{
    if (s.size() != len) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/** True if the pubkey's X coordinate is all zeros, i.e. an unset placeholder.
 *  Accepts both a bare 64-hex x-only key and a 66-hex key whose prefix byte is
 *  followed by 64 zeros. */
bool IsPlaceholderPubkey(const std::string& s)
{
    const size_t n = s.size();
    if (n != 64 && n != 66) return false;
    return s.compare(n - 64, 64, std::string(64, '0')) == 0;
}

bool IsAsciiAlnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/** Hostname syntax the registry accepts: dot-separated labels of alphanumerics
 *  with single interior hyphens, and an alphabetic TLD of two or more. */
bool IsValidDomain(const std::string& d)
{
    if (d.empty() || d.size() > 253) return false;
    if (d.front() == '.' || d.back() == '.') return false;

    size_t start = 0;
    std::vector<std::string> labels;
    while (true) {
        const size_t dot = d.find('.', start);
        const std::string label = d.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (label.empty()) return false;
        labels.push_back(label);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (labels.size() < 2) return false; // a bare hostname names no registrable domain

    for (size_t i = 0; i < labels.size(); ++i) {
        const std::string& label = labels[i];
        if (!IsAsciiAlnum(label.front()) || !IsAsciiAlnum(label.back())) return false;
        for (size_t j = 0; j < label.size(); ++j) {
            const char c = label[j];
            if (IsAsciiAlnum(c)) continue;
            if (c != '-') return false;
            if (j + 1 < label.size() && label[j + 1] == '-') return false; // no doubled hyphens
        }
    }

    const std::string& tld = labels.back();
    if (tld.size() < 2) return false;
    for (const char c : tld) {
        if (!std::isalpha(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

/** Reject anything the two JSON writers might escape differently, plus DEL. */
bool HasControlChars(const std::string& s)
{
    for (const unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

} // namespace

std::string CanonicalJson(const UniValue& value)
{
    std::string out;
    AppendCanonical(value, out);
    return out;
}

uint256 Hash(const UniValue& contract)
{
    const std::string canonical = CanonicalJson(contract);
    uint256 out;
    CSHA256()
        .Write(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size())
        .Finalize(out.begin());
    return out;
}

UniValue Build(const std::string& name,
               const std::string& ticker,
               int precision,
               const std::string& domain,
               const std::string& issuer_pubkey)
{
    // Domain and pubkey are normalised to lower case: the registry matches its
    // pubkey against a lower-case-only pattern, and hostnames are
    // case-insensitive anyway, so folding here saves an issuer from committing a
    // contract that only fails once it is too late to change.
    std::string lower_domain = domain;
    std::transform(lower_domain.begin(), lower_domain.end(), lower_domain.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string lower_pubkey = issuer_pubkey;
    std::transform(lower_pubkey.begin(), lower_pubkey.end(), lower_pubkey.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    UniValue entity(UniValue::VOBJ);
    entity.pushKV("domain", lower_domain);

    UniValue contract(UniValue::VOBJ);
    contract.pushKV("entity", entity);
    contract.pushKV("issuer_pubkey", lower_pubkey);
    contract.pushKV("name", name);
    contract.pushKV("precision", precision);
    contract.pushKV("ticker", ticker);
    contract.pushKV("version", 0);
    return contract;
}

std::vector<std::string> Validate(const UniValue& contract)
{
    std::vector<std::string> errors;
    if (!contract.isObject()) {
        errors.push_back("contract must be a JSON object");
        return errors;
    }

    // name
    const UniValue& name = contract["name"];
    if (!name.isStr() || name.get_str().empty() || name.get_str().size() > 255) {
        errors.push_back("name: must be 1 to 255 characters");
    } else if (HasControlChars(name.get_str())) {
        errors.push_back("name: must not contain control characters");
    }

    // ticker
    const UniValue& ticker = contract["ticker"];
    if (!ticker.isStr()) {
        errors.push_back("ticker: must be a string");
    } else {
        const std::string t = ticker.get_str();
        bool ok = !t.empty() && t.size() <= 12;
        for (const char c : t) {
            if (!IsAsciiAlnum(c) && c != '.' && c != '-') ok = false;
        }
        if (!ok) errors.push_back("ticker: 1 to 12 characters of A-Z, a-z, 0-9, '.' or '-'");
    }

    // precision
    const UniValue& precision = contract["precision"];
    if (!precision.isNum() || precision.get_int() < 0 || precision.get_int() > 8) {
        errors.push_back("precision: whole number from 0 to 8");
    }

    // entity.domain
    const UniValue& entity = contract["entity"];
    if (!entity.isObject()) {
        errors.push_back("entity: must be an object holding the issuer domain");
    } else {
        const UniValue& domain = entity["domain"];
        if (!domain.isStr() || !IsValidDomain(domain.get_str())) {
            errors.push_back("entity.domain: must be a domain name you control, such as example.com");
        }
        for (const std::string& k : entity.getKeys()) {
            if (k != "domain" && k != "issuer") errors.push_back(strprintf("entity: unexpected field '%s'", k));
        }
        if (entity.exists("issuer") && !entity["issuer"].isStr()) {
            errors.push_back("entity.issuer: must be a string");
        }
    }

    // issuer_pubkey
    const UniValue& pubkey = contract["issuer_pubkey"];
    if (!pubkey.isStr() || !(IsLowerHex(pubkey.get_str(), 66) || IsLowerHex(pubkey.get_str(), 64))) {
        errors.push_back("issuer_pubkey: 33-byte compressed or 32-byte x-only key, as lower-case hex");
    } else if (IsPlaceholderPubkey(pubkey.get_str())) {
        errors.push_back("issuer_pubkey: must be a real key, not an all-zeros placeholder");
    }

    // version
    const UniValue& version = contract["version"];
    if (!version.isNum() || version.get_int() != 0) {
        errors.push_back("version: must be 0");
    }

    static const std::set<std::string> allowed{
        "name", "ticker", "precision", "entity", "issuer_pubkey", "version", "openamp", "operator"};
    for (const std::string& k : contract.getKeys()) {
        if (!allowed.count(k)) errors.push_back(strprintf("unexpected field '%s'", k));
    }

    return errors;
}

std::string AssetProofLine(const std::string& domain, const std::string& asset_id)
{
    return strprintf("Authorize linking the domain name %s to the Sequentia asset %s", domain, asset_id);
}

std::string AssetProofPath(const std::string& asset_id)
{
    return strprintf(".well-known/sequentia-asset-proof-%s", asset_id);
}

std::string AssetProofUrl(const std::string& domain, const std::string& asset_id)
{
    return strprintf("https://%s/%s", domain, AssetProofPath(asset_id));
}

} // namespace AssetContract
