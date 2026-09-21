/**
 * @file ChunkedDecoder.h
 * @brief Incremental decoder for the `aws-chunked` body encoding.
 *
 * Wire format (one chunk):  <hex size>[;chunk-signature=<64 hex>]\r\n<data>\r\n
 * ended by a zero-size chunk, then optional trailer lines
 * (`x-amz-checksum-crc32:<b64>\r\n`, and for the signed variant
 * `x-amz-trailer-signature:<hex>\r\n`) and a final blank line.
 *
 * feed() takes input in any slicing and hands decoded bytes to a sink as they
 * appear, so memory stays bounded by the largest slice fed, never by the body.
 * For signed streams every chunk signature is verified against the chain
 * seeded by the request signature. The sink sees a chunk's bytes before its
 * signature is checked (only the header line is buffered), so callers must
 * discard what they wrote if feed() reports an error.
 */

#pragma once

#include "Crypto.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace s3::sigv4
{

class ChunkedDecoder
{
  public:
    struct Params {
        bool signedChunks = false; ///< STREAMING-AWS4-HMAC-SHA256-PAYLOAD[-TRAILER]
        bool trailer = false;      ///< a trailer section follows the last chunk
        std::string signingKey;    ///< raw, signed variant only
        std::string amzDate;
        std::string scope;
        std::string seedSignature; ///< the request's own signature
    };
    enum class Status { Ok, Malformed, BadSignature };
    using Sink = std::function<void(std::string_view)>;

    explicit ChunkedDecoder(Params p);

    /// @brief Consume `in`. After an error the decoder stays failed.
    Status feed(std::string_view in, const Sink& sink);

    bool done() const { return state_ == State::Done; }
    uint64_t decodedBytes() const { return decoded_; }
    /// Trailer headers (lower-case name, value), excluding the signature.
    const std::vector<std::pair<std::string, std::string>>& trailers() const
    {
        return trailers_;
    }
    const std::string& error() const { return error_; }

  private:
    enum class State { Header, Data, DataEnd, Trailer, Done, Failed };

    Status fail(Status s, std::string msg);
    Status onHeaderLine();
    Status onChunkEnd();
    Status onTrailerLine();

    Params p_;
    State state_ = State::Header;
    std::string line_;
    uint64_t remaining_ = 0;
    uint64_t decoded_ = 0;
    int crlfSeen_ = 0;
    std::string claimedSig_;
    std::string prevSig_;
    Digest chunkHash_;
    bool lastChunkSeen_ = false;
    bool sawTrailerSig_ = false;
    std::string canonTrailer_;
    std::vector<std::pair<std::string, std::string>> trailers_;
    std::string error_;
};

} // namespace s3::sigv4
