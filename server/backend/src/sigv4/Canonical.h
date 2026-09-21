/**
 * @file Canonical.h
 * @brief SigV4 canonical request, string to sign and signing key (pure).
 *
 * Follows the S3 variant of the rules: the path is URI-encoded once (never
 * twice), the payload hash comes from x-amz-content-sha256 (or the literal
 * UNSIGNED-PAYLOAD for presigned URLs), and the canonical query is the
 * decoded-then-re-encoded, sorted query without X-Amz-Signature.
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace s3::sigv4
{

/// Lower-cased header name -> raw value.
using HeaderMap = std::map<std::string, std::string>;

struct CanonicalInput {
    std::string method;
    std::string rawPath;  ///< as received on the request line (percent-encoded)
    std::string rawQuery; ///< as received, without '?'
    const HeaderMap* headers = nullptr;
    std::vector<std::string> signedHeaders; ///< lower-case
    std::string payloadHash;
    bool dropSignatureParam = false; ///< presigned: omit X-Amz-Signature
};

/// @brief Decode the request path and encode it once more, the S3 way.
std::string canonicalUri(std::string_view rawPath);

/// @brief Sorted, re-encoded query; `dropSignature` removes X-Amz-Signature.
std::string canonicalQuery(std::string_view rawQuery, bool dropSignature);

/// @brief Header values trimmed, inner whitespace runs collapsed to one space.
std::string trimHeaderValue(std::string_view v);

/// @brief The full canonical request; nullopt if a signed header is missing.
std::optional<std::string> canonicalRequest(const CanonicalInput& in);

std::string stringToSign(std::string_view amzDate, std::string_view scope,
                         std::string_view canonicalRequest);

/// @brief Raw HMAC chain key for (secret, yyyymmdd, region, service).
std::string deriveSigningKey(std::string_view secret, std::string_view date,
                             std::string_view region,
                             std::string_view service = "s3");

/// @brief Hex HMAC-SHA256 of the string to sign.
std::string signatureHex(std::string_view signingKey,
                         std::string_view stringToSign);

} // namespace s3::sigv4
