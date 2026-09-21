/**
 * @file PayloadReader.h
 * @brief Turns a request body into the bytes the client meant to store, and
 *        checks every integrity claim made about them.
 *
 * Handles the x-amz-content-sha256 modes (hex digest, UNSIGNED-PAYLOAD and the
 * four STREAMING-* aws-chunked variants), x-amz-decoded-content-length,
 * Content-MD5 and x-amz-checksum-* (header or trailer). An aws-chunked body is
 * decoded incrementally from the (possibly mmapped, spooled) request body into
 * a file under tmpDir when it is large, so memory stays bounded; a plain body
 * is used in place.
 */

#pragma once

#include "Canonical.h"
#include "ChunkedDecoder.h"
#include "SigV4Verify.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace s3
{
class MappedFile;
}

namespace s3::sigv4
{

struct Payload {
    std::string_view view; ///< the decoded bytes
    /// A checksum the client supplied and we verified: {header name, b64}.
    std::optional<std::pair<std::string, std::string>> checksum;
    /// Content-Encoding with the aws-chunked token removed (may be empty).
    std::string contentEncoding;

    Payload();
    ~Payload();
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;

    std::string memory;
    std::unique_ptr<MappedFile> file;
};

struct PayloadOptions {
    std::string payloadHash; ///< x-amz-content-sha256 (or UNSIGNED-PAYLOAD)
    const HeaderMap* headers = nullptr;
    std::filesystem::path tmpDir;
    uint64_t maxDecodedBytes = 0;
    size_t memoryLimit = 4 * 1024 * 1024;
    /// False for POST: on CompleteMultipartUpload x-amz-checksum-* describes
    /// the assembled object, not the XML body.
    bool checkChecksumHeaders = true;
    /// Chain seed for the signed streaming variants.
    ChunkedDecoder::Params chunk;
};

struct PayloadResult {
    std::optional<AuthError> error;
    std::shared_ptr<Payload> payload;
};

/// @brief Is this x-amz-content-sha256 value an aws-chunked mode?
bool isStreamingPayloadHash(std::string_view h);

PayloadResult preparePayload(std::string_view body, const PayloadOptions& opt);

} // namespace s3::sigv4
