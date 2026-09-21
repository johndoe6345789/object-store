/**
 * @file SigV4Verify.cpp
 * @brief Signature verification. See SigV4Verify.h.
 */

#include "SigV4Verify.h"

#include "../services/AuthUtil.h"
#include "Crypto.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>

namespace s3::sigv4
{

std::optional<time_t> parseAmzDate(std::string_view s)
{
    if (s.size() != 16 || s[8] != 'T' || s[15] != 'Z')
        return std::nullopt;
    auto num = [&](size_t off, size_t len) -> int {
        int v = 0;
        for (size_t i = off; i < off + len; ++i) {
            if (s[i] < '0' || s[i] > '9')
                return -1;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    int y = num(0, 4), mo = num(4, 2), d = num(6, 2), h = num(9, 2),
        mi = num(11, 2), se = num(13, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || se < 0 || se > 60)
        return std::nullopt;
    struct tm tmv {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    return timegm(&tmv);
}

std::optional<time_t> parseHttpDate(std::string_view s)
{
    // "Fri, 24 May 2013 00:00:00 GMT" -- parsed by hand so the C locale is
    // not needed for the month/day names.
    static const char* mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (s.size() != 29 || s[3] != ',' || s.substr(25) != " GMT")
        return std::nullopt;
    int mon = -1;
    for (int i = 0; i < 12; ++i)
        if (s.substr(8, 3) == mons[i])
            mon = i;
    if (mon < 0)
        return std::nullopt;
    auto num = [&](size_t off, size_t len) -> int {
        int v = 0;
        for (size_t i = off; i < off + len; ++i) {
            if (s[i] < '0' || s[i] > '9')
                return -1;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    int d = num(5, 2), y = num(12, 4), h = num(17, 2), mi = num(20, 2),
        se = num(23, 2);
    if (d < 1 || y < 1970 || h < 0 || mi < 0 || se < 0)
        return std::nullopt;
    struct tm tmv {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mon;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    return timegm(&tmv);
}

namespace {

/// x-amz-* headers the store acts on: they must be covered by the signature,
/// or whoever can touch the request in flight could redirect a copy or swap
/// metadata without invalidating it.
bool mustBeSigned(const std::string& name)
{
    static const char* prefixes[] = {"x-amz-copy-source", "x-amz-meta-",
                                     "x-amz-checksum-", "x-amz-metadata-directive"};
    static const char* exact[] = {"x-amz-content-sha256",
                                  "x-amz-decoded-content-length", "x-amz-trailer",
                                  "x-amz-sdk-checksum-algorithm", "x-amz-date"};
    for (auto p : prefixes)
        if (name.rfind(p, 0) == 0)
            return true;
    for (auto e : exact)
        if (name == e)
            return true;
    return false;
}

std::string formatAmzDate(time_t t)
{
    struct tm tmv {};
    gmtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02dT%02d%02d%02dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                  tmv.tm_min, tmv.tm_sec);
    return buf;
}

} // namespace

VerifyOutcome verify(const ParsedAuth& p, const std::string& secret,
                     const VerifyInput& in, const VerifyConfig& cfg)
{
    VerifyOutcome o;
    auto fail = [&](int status, const char* code, std::string msg) {
        o.error = AuthError{status, code, std::move(msg)};
        return o;
    };
    const auto& h = *in.headers;
    auto has = [&](const std::string& n) {
        return std::find(p.signedHeaders.begin(), p.signedHeaders.end(), n) !=
               p.signedHeaders.end();
    };

    if (!cfg.anyRegion && p.region != cfg.region)
        return fail(400, "AuthorizationHeaderMalformed",
                    "The authorization header is malformed; the region '" +
                        p.region + "' is wrong; expecting '" + cfg.region + "'");

    // ---- request time ------------------------------------------------
    std::string amzDate;
    if (p.presigned) {
        amzDate = p.amzDate;
    } else if (auto it = h.find("x-amz-date"); it != h.end()) {
        amzDate = it->second;
    } else if (auto d = h.find("date"); d != h.end()) {
        auto t = parseHttpDate(d->second);
        if (!t)
            return fail(403, "AccessDenied", "Invalid Date header");
        amzDate = formatAmzDate(*t);
    } else {
        return fail(403, "AccessDenied",
                    "AWS authentication requires a valid Date or x-amz-date "
                    "header");
    }
    auto t = parseAmzDate(amzDate);
    if (!t)
        return fail(400, "AuthorizationHeaderMalformed",
                    "Invalid x-amz-date; expected YYYYMMDD'T'HHMMSS'Z'");
    if (amzDate.compare(0, 8, p.dateStamp) != 0)
        return fail(400, "AuthorizationHeaderMalformed",
                    "The authorization header is malformed; Invalid credential "
                    "date. Date is not the same as X-Amz-Date.");
    if (p.presigned) {
        if (in.now + cfg.maxSkewSeconds < *t)
            return fail(403, "AccessDenied", "Request is not yet valid");
        if (in.now > *t + p.expires)
            return fail(403, "AccessDenied", "Request has expired");
    } else if (std::labs(static_cast<long>(in.now - *t)) > cfg.maxSkewSeconds) {
        return fail(403, "RequestTimeTooSkewed",
                    "The difference between the request time and the current "
                    "time is too large.");
    }

    // ---- signed-header rules -----------------------------------------
    if (!has("host"))
        return fail(403, "AccessDenied", "The host header must be signed");
    for (auto& [name, _] : h)
        if (mustBeSigned(name) && !has(name) && !(p.presigned && name == "x-amz-date"))
            return fail(403, "AccessDenied",
                        "There were headers present in the request which were "
                        "not signed: " + name);
    if (p.presigned) {
        o.payloadHash = "UNSIGNED-PAYLOAD";
    } else {
        auto it = h.find("x-amz-content-sha256");
        if (it == h.end())
            return fail(400, "InvalidRequest",
                        "Missing required header for this request: "
                        "x-amz-content-sha256");
        if (!has("x-amz-content-sha256") ||
            !(has("x-amz-date") || has("date")))
            return fail(403, "AccessDenied",
                        "x-amz-content-sha256 and x-amz-date must be signed");
        o.payloadHash = it->second;
    }

    // ---- signature -----------------------------------------------------
    CanonicalInput ci;
    ci.method = in.method;
    ci.rawPath = in.rawPath;
    ci.rawQuery = in.rawQuery;
    ci.headers = &h;
    ci.signedHeaders = p.signedHeaders;
    ci.payloadHash = o.payloadHash;
    ci.dropSignatureParam = p.presigned;
    auto canon = canonicalRequest(ci);
    const char* badSig =
        "The request signature we calculated does not match the signature you "
        "provided. Check your key and signing method.";
    if (!canon)
        return fail(403, "SignatureDoesNotMatch", badSig);
    o.scope = p.dateStamp + "/" + p.region + "/s3/aws4_request";
    o.amzDate = amzDate;
    o.signingKey = deriveSigningKey(secret, p.dateStamp, p.region, "s3");
    auto expected =
        signatureHex(o.signingKey, stringToSign(amzDate, o.scope, *canon));
    if (!constantTimeEquals(expected, p.signature))
        return fail(403, "SignatureDoesNotMatch", badSig);
    o.signature = expected;
    return o;
}

} // namespace s3::sigv4
