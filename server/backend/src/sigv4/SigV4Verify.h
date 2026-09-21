/**
 * @file SigV4Verify.h
 * @brief Parse and verify AWS Signature V4 (Authorization header and
 *        presigned query string). No drogon, no DB: unit-testable.
 */

#pragma once

#include "Canonical.h"

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace s3::sigv4
{

/// An S3-style failure: HTTP status, error code, human message.
struct AuthError {
    int status = 403;
    std::string code;
    std::string message;
};

struct ParsedAuth {
    bool presigned = false;
    std::string accessKey;
    std::string dateStamp; ///< yyyymmdd from the credential scope
    std::string region;
    std::string service;
    std::vector<std::string> signedHeaders; ///< lower-case
    std::string signature;                  ///< lower-case hex, 64 chars
    std::string amzDate;                    ///< presigned only: X-Amz-Date
    long expires = 0;                       ///< presigned only, seconds
};

struct VerifyConfig {
    std::string region = "us-east-1";
    bool anyRegion = false;  ///< S3_ANY_REGION=1: accept any credential region
    long maxSkewSeconds = 900;
};

struct VerifyInput {
    std::string method;
    std::string rawPath;
    std::string rawQuery;
    const HeaderMap* headers = nullptr;
    time_t now = 0;
};

struct VerifyOutcome {
    std::optional<AuthError> error;
    std::string signingKey; ///< raw key (for the aws-chunked signature chain)
    std::string scope;      ///< date/region/s3/aws4_request
    std::string amzDate;
    std::string signature;  ///< the (verified) request signature
    std::string payloadHash;
};

constexpr long kMaxPresignSeconds = 604800; // 7 days

/// @brief Does the query carry presigned-URL authentication?
bool isPresignedQuery(std::string_view rawQuery);

/// @brief Parse `AWS4-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...`.
std::optional<AuthError> parseAuthorizationHeader(std::string_view value,
                                                  ParsedAuth& out);

/// @brief Parse X-Amz-Algorithm/Credential/Date/Expires/SignedHeaders/Signature.
std::optional<AuthError> parsePresignedQuery(std::string_view rawQuery,
                                             ParsedAuth& out);

/// @brief Parse 20130524T000000Z / an RFC 7231 Date into epoch seconds.
std::optional<time_t> parseAmzDate(std::string_view s);
std::optional<time_t> parseHttpDate(std::string_view s);

/// @brief Full check: scope, clock, expiry, signed-header rules and signature.
///        `secret` is the plaintext secret of the access key.
VerifyOutcome verify(const ParsedAuth& parsed, const std::string& secret,
                     const VerifyInput& in, const VerifyConfig& cfg);

} // namespace s3::sigv4
