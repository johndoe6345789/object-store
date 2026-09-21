/**
 * @file PresignedAuth.cpp
 * @brief Parsing of SigV4 authentication: header form and presigned query.
 */

#include "SigV4Verify.h"

#include "UriEncoding.h"

#include <cctype>
#include <cstring>

namespace s3::sigv4
{

namespace {

AuthError malformed(const std::string& msg)
{
    return {400, "AuthorizationHeaderMalformed", msg};
}

std::vector<std::string> splitOn(std::string_view s, char sep)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string_view::npos)
            j = s.size();
        out.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool isHex64(std::string_view s)
{
    if (s.size() != 64)
        return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Credential = <access>/<yyyymmdd>/<region>/<service>/aws4_request. The
/// access key is everything before the last four components.
std::optional<AuthError> parseCredential(std::string_view cred, ParsedAuth& o,
                                         AuthError (*err)(const std::string&))
{
    auto parts = splitOn(cred, '/');
    if (parts.size() < 5)
        return err("The authorization header is malformed; the Credential is "
                   "mal-formed; expecting \"<YOUR-AKID>/YYYYMMDD/REGION/"
                   "SERVICE/aws4_request\".");
    const size_t n = parts.size();
    if (parts[n - 1] != "aws4_request")
        return err("The authorization header is malformed; incorrect "
                   "terminal \"" + parts[n - 1] + "\". This endpoint uses "
                   "\"aws4_request\".");
    o.service = parts[n - 2];
    o.region = parts[n - 3];
    o.dateStamp = parts[n - 4];
    o.accessKey.clear();
    for (size_t i = 0; i + 4 < n; ++i)
        o.accessKey += (i ? "/" : "") + parts[i];
    if (o.accessKey.empty() || o.dateStamp.size() != 8)
        return err("The authorization header is malformed; the Credential is "
                   "mal-formed.");
    for (char c : o.dateStamp)
        if (c < '0' || c > '9')
            return err("The authorization header is malformed; invalid "
                       "credential date.");
    if (o.service != "s3")
        return err("The authorization header is malformed; incorrect "
                   "service \"" + o.service + "\". This endpoint belongs to "
                   "\"s3\".");
    return std::nullopt;
}

} // namespace

bool isPresignedQuery(std::string_view q)
{
    size_t i = 0;
    while (i <= q.size()) {
        size_t j = q.find('&', i);
        if (j == std::string_view::npos)
            j = q.size();
        auto pair = q.substr(i, j - i);
        auto k = pair.substr(0, pair.find('='));
        if (k == "X-Amz-Algorithm" || k == "X-Amz-Signature")
            return true;
        i = j + 1;
    }
    return false;
}

std::optional<AuthError> parseAuthorizationHeader(std::string_view v,
                                                  ParsedAuth& out)
{
    constexpr std::string_view kAlgo = "AWS4-HMAC-SHA256";
    if (v.substr(0, kAlgo.size()) != kAlgo ||
        (v.size() > kAlgo.size() && v[kAlgo.size()] != ' '))
        return AuthError{403, "AuthorizationHeaderMalformed",
                         "Unsupported Authorization Type: only AWS Signature "
                         "Version 4 (AWS4-HMAC-SHA256) is accepted."};
    out = ParsedAuth{};
    std::string cred, signedH, sig;
    for (auto& field : splitOn(v.substr(kAlgo.size()), ',')) {
        size_t b = field.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        auto f = std::string_view(field).substr(b);
        auto eq = f.find('=');
        if (eq == std::string_view::npos)
            return malformed("The authorization header is malformed; the "
                             "authorization component \"" + std::string(f) +
                             "\" is malformed.");
        auto k = f.substr(0, eq);
        auto val = std::string(f.substr(eq + 1));
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        if (k == "Credential")
            cred = val;
        else if (k == "SignedHeaders")
            signedH = val;
        else if (k == "Signature")
            sig = val;
    }
    if (cred.empty() || signedH.empty() || sig.empty())
        return malformed("The authorization header is malformed; Authorization "
                         "header requires Credential, SignedHeaders and "
                         "Signature parameters.");
    if (auto e = parseCredential(cred, out, malformed))
        return e;
    for (auto& h : splitOn(signedH, ';'))
        if (!h.empty())
            out.signedHeaders.push_back(lower(h));
    if (out.signedHeaders.empty())
        return malformed("The authorization header is malformed; empty "
                         "SignedHeaders.");
    if (!isHex64(sig))
        return malformed("The authorization header is malformed; the "
                         "Signature must be 64 hex characters.");
    out.signature = lower(sig);
    return std::nullopt;
}

std::optional<AuthError> parsePresignedQuery(std::string_view q, ParsedAuth& out)
{
    auto qerr = [](const std::string& m) -> AuthError {
        return {400, "AuthorizationQueryParametersError", m};
    };
    out = ParsedAuth{};
    out.presigned = true;
    std::string algo, cred, signedH, sig, expires;
    size_t i = 0;
    while (i <= q.size() && !q.empty()) {
        size_t j = q.find('&', i);
        if (j == std::string_view::npos)
            j = q.size();
        auto pair = q.substr(i, j - i);
        i = j + 1;
        auto eq = pair.find('=');
        auto k = uriDecode(pair.substr(0, eq), true);
        auto val = eq == std::string_view::npos
                       ? std::string()
                       : uriDecode(pair.substr(eq + 1), true);
        if (k == "X-Amz-Algorithm")
            algo = val;
        else if (k == "X-Amz-Credential")
            cred = val;
        else if (k == "X-Amz-Date")
            out.amzDate = val;
        else if (k == "X-Amz-Expires")
            expires = val;
        else if (k == "X-Amz-SignedHeaders")
            signedH = val;
        else if (k == "X-Amz-Signature")
            sig = val;
    }
    if (algo != "AWS4-HMAC-SHA256")
        return qerr("X-Amz-Algorithm only supports \"AWS4-HMAC-SHA256\"");
    if (cred.empty() || out.amzDate.empty() || expires.empty() ||
        signedH.empty() || sig.empty())
        return qerr("Query-string authentication requires the X-Amz-Algorithm, "
                    "X-Amz-Credential, X-Amz-Signature, X-Amz-Date, "
                    "X-Amz-SignedHeaders, and X-Amz-Expires parameters.");
    if (auto e = parseCredential(cred, out, qerr))
        return e;
    long ex = 0;
    if (expires.size() > 9)
        ex = -1;
    else
        for (char c : expires) {
            if (c < '0' || c > '9') {
                ex = -1;
                break;
            }
            ex = ex * 10 + (c - '0');
        }
    if (ex < 1)
        return qerr("X-Amz-Expires must be a positive integer number of seconds");
    if (ex > kMaxPresignSeconds)
        return qerr("X-Amz-Expires must be less than a week (in seconds); "
                    "that is, the given X-Amz-Expires must be less than "
                    "604800 seconds");
    out.expires = ex;
    for (auto& h : splitOn(signedH, ';'))
        if (!h.empty())
            out.signedHeaders.push_back(lower(h));
    if (!isHex64(sig))
        return qerr("The X-Amz-Signature must be 64 hex characters.");
    out.signature = lower(sig);
    return std::nullopt;
}

} // namespace s3::sigv4
