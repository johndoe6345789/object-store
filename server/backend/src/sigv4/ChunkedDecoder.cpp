/**
 * @file ChunkedDecoder.cpp
 * @brief aws-chunked decoding and chunk-signature chain. See the header.
 */

#include "ChunkedDecoder.h"

#include "../services/AuthUtil.h"

#include <algorithm>
#include <cctype>

namespace s3::sigv4
{

namespace {
constexpr size_t kMaxLine = 4096;
const std::string& emptyHash()
{
    static const std::string h = sha256Hex("");
    return h;
}
} // namespace

ChunkedDecoder::ChunkedDecoder(Params p)
    : p_(std::move(p)), prevSig_(p_.seedSignature), chunkHash_(EVP_sha256())
{
}

ChunkedDecoder::Status ChunkedDecoder::fail(Status s, std::string msg)
{
    state_ = State::Failed;
    error_ = std::move(msg);
    return s;
}

ChunkedDecoder::Status ChunkedDecoder::feed(std::string_view in,
                                            const Sink& sink)
{
    while (!in.empty()) {
        switch (state_) {
        case State::Failed:
            return Status::Malformed;
        case State::Done:
            for (char c : in)
                if (c != '\r' && c != '\n')
                    return fail(Status::Malformed, "data after final chunk");
            return Status::Ok;
        case State::Header:
        case State::Trailer: {
            auto nl = in.find('\n');
            auto part = in.substr(0, nl == std::string_view::npos ? in.size() : nl);
            if (line_.size() + part.size() > kMaxLine)
                return fail(Status::Malformed, "chunk header too long");
            line_.append(part);
            if (nl == std::string_view::npos) {
                in = {};
                break;
            }
            in.remove_prefix(nl + 1);
            if (line_.empty() || line_.back() != '\r')
                return fail(Status::Malformed, "chunk header not CRLF-terminated");
            line_.pop_back();
            auto st = state_ == State::Header ? onHeaderLine() : onTrailerLine();
            line_.clear();
            if (st != Status::Ok)
                return st;
            break;
        }
        case State::Data: {
            auto n = static_cast<size_t>(
                std::min<uint64_t>(remaining_, in.size()));
            auto d = in.substr(0, n);
            sink(d);
            if (p_.signedChunks)
                chunkHash_.update(d);
            decoded_ += n;
            remaining_ -= n;
            in.remove_prefix(n);
            if (remaining_ == 0) {
                if (auto st = onChunkEnd(); st != Status::Ok)
                    return st;
                state_ = State::DataEnd;
                crlfSeen_ = 0;
            }
            break;
        }
        case State::DataEnd: {
            const char want = crlfSeen_ == 0 ? '\r' : '\n';
            if (in.front() != want)
                return fail(Status::Malformed, "missing CRLF after chunk data");
            in.remove_prefix(1);
            if (++crlfSeen_ == 2)
                state_ = State::Header;
            break;
        }
        }
    }
    return Status::Ok;
}

ChunkedDecoder::Status ChunkedDecoder::onHeaderLine()
{
    std::string_view l = line_;
    auto semi = l.find(';');
    auto hex = l.substr(0, semi);
    if (hex.empty() || hex.size() > 16)
        return fail(Status::Malformed, "bad chunk size");
    uint64_t size = 0;
    for (char c : hex) {
        int v = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (v < 0)
            return fail(Status::Malformed, "bad chunk size");
        size = size * 16 + static_cast<uint64_t>(v);
    }
    claimedSig_.clear();
    if (semi != std::string_view::npos) {
        constexpr std::string_view k = "chunk-signature=";
        auto ext = l.substr(semi + 1);
        if (ext.substr(0, k.size()) == k)
            claimedSig_ = std::string(ext.substr(k.size()));
    }
    if (p_.signedChunks && claimedSig_.size() != 64)
        return fail(Status::Malformed, "missing chunk-signature");
    remaining_ = size;
    chunkHash_.reset(EVP_sha256());
    if (size == 0) {
        lastChunkSeen_ = true;
        if (auto st = onChunkEnd(); st != Status::Ok)
            return st;
        // A trailer section (possibly just the blank line) follows.
        state_ = State::Trailer;
    } else {
        state_ = State::Data;
    }
    return Status::Ok;
}

ChunkedDecoder::Status ChunkedDecoder::onChunkEnd()
{
    if (!p_.signedChunks)
        return Status::Ok;
    auto sts = "AWS4-HMAC-SHA256-PAYLOAD\n" + p_.amzDate + "\n" + p_.scope +
               "\n" + prevSig_ + "\n" + emptyHash() + "\n" +
               hexEncode(chunkHash_.finish());
    auto expect = hexEncode(hmacSha256(p_.signingKey, sts));
    std::string claimed = claimedSig_;
    for (auto& c : claimed)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!constantTimeEquals(expect, claimed))
        return fail(Status::BadSignature, "chunk signature mismatch");
    prevSig_ = expect;
    return Status::Ok;
}

ChunkedDecoder::Status ChunkedDecoder::onTrailerLine()
{
    if (line_.empty()) {
        if (p_.trailer && p_.signedChunks && !sawTrailerSig_)
            return fail(Status::BadSignature, "missing trailer signature");
        state_ = State::Done;
        return Status::Ok;
    }
    if (!p_.trailer)
        return fail(Status::Malformed, "unexpected trailer");
    auto colon = line_.find(':');
    if (colon == std::string::npos)
        return fail(Status::Malformed, "bad trailer line");
    std::string name = line_.substr(0, colon);
    for (auto& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string value = line_.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
        value.erase(value.begin());
    if (name == "x-amz-trailer-signature") {
        if (!p_.signedChunks)
            return Status::Ok;
        sawTrailerSig_ = true;
        auto sts = "AWS4-HMAC-SHA256-TRAILER\n" + p_.amzDate + "\n" +
                   p_.scope + "\n" + prevSig_ + "\n" + sha256Hex(canonTrailer_);
        auto expect = hexEncode(hmacSha256(p_.signingKey, sts));
        for (auto& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!constantTimeEquals(expect, value))
            return fail(Status::BadSignature, "trailer signature mismatch");
        return Status::Ok;
    }
    canonTrailer_ += name + ":" + value + "\n";
    trailers_.emplace_back(std::move(name), std::move(value));
    return Status::Ok;
}

} // namespace s3::sigv4
