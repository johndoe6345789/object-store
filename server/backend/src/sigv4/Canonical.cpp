/**
 * @file Canonical.cpp
 * @brief SigV4 canonicalisation. See Canonical.h.
 */

#include "Canonical.h"

#include "Crypto.h"
#include "UriEncoding.h"

#include <algorithm>

namespace s3::sigv4
{

std::string canonicalUri(std::string_view rawPath)
{
    if (rawPath.empty())
        return "/";
    return uriEncode(uriDecode(rawPath, false), false);
}

std::string canonicalQuery(std::string_view rawQuery, bool dropSignature)
{
    std::vector<std::pair<std::string, std::string>> kv;
    size_t i = 0;
    while (i <= rawQuery.size() && !rawQuery.empty()) {
        size_t j = rawQuery.find('&', i);
        if (j == std::string_view::npos)
            j = rawQuery.size();
        auto pair = rawQuery.substr(i, j - i);
        i = j + 1;
        if (pair.empty())
            continue;
        auto eq = pair.find('=');
        auto k = uriDecode(pair.substr(0, eq), true);
        auto v = eq == std::string_view::npos
                     ? std::string()
                     : uriDecode(pair.substr(eq + 1), true);
        if (dropSignature && k == "X-Amz-Signature")
            continue;
        kv.emplace_back(uriEncode(k, true), uriEncode(v, true));
    }
    std::sort(kv.begin(), kv.end());
    std::string out;
    for (auto& [k, v] : kv) {
        if (!out.empty())
            out += '&';
        out += k + "=" + v;
    }
    return out;
}

std::string trimHeaderValue(std::string_view v)
{
    std::string out;
    bool space = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::optional<std::string> canonicalRequest(const CanonicalInput& in)
{
    auto names = in.signedHeaders;
    std::sort(names.begin(), names.end());
    std::string headers;
    for (auto& n : names) {
        auto it = in.headers->find(n);
        if (it == in.headers->end())
            return std::nullopt;
        headers += n + ":" + trimHeaderValue(it->second) + "\n";
    }
    std::string signedList;
    for (auto& n : names) {
        if (!signedList.empty())
            signedList += ';';
        signedList += n;
    }
    return in.method + "\n" + canonicalUri(in.rawPath) + "\n" +
           canonicalQuery(in.rawQuery, in.dropSignatureParam) + "\n" + headers +
           "\n" + signedList + "\n" + in.payloadHash;
}

std::string stringToSign(std::string_view amzDate, std::string_view scope,
                         std::string_view canonicalRequest)
{
    return "AWS4-HMAC-SHA256\n" + std::string(amzDate) + "\n" +
           std::string(scope) + "\n" + sha256Hex(canonicalRequest);
}

std::string deriveSigningKey(std::string_view secret, std::string_view date,
                             std::string_view region, std::string_view service)
{
    auto k = hmacSha256("AWS4" + std::string(secret), date);
    k = hmacSha256(k, region);
    k = hmacSha256(k, service);
    return hmacSha256(k, "aws4_request");
}

std::string signatureHex(std::string_view signingKey,
                         std::string_view stringToSign)
{
    return hexEncode(hmacSha256(signingKey, stringToSign));
}

} // namespace s3::sigv4
