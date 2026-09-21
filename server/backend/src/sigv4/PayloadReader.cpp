/**
 * @file PayloadReader.cpp
 * @brief See PayloadReader.h.
 */

#include "PayloadReader.h"

#include "../services/DigestUtil.h"
#include "../services/MappedFile.h"
#include "Checksum.h"
#include "Crypto.h"

#include <fstream>
#include <map>

namespace s3::sigv4
{

Payload::Payload() = default;
Payload::~Payload() = default;

namespace {

constexpr size_t kSlice = 1 << 20;

bool isHex64(std::string_view s)
{
    if (s.size() != 64)
        return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

AuthError err(int status, const char* code, std::string msg)
{
    return {status, code, std::move(msg)};
}

std::string upper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string headerOr(const HeaderMap& h, const char* name)
{
    auto it = h.find(name);
    return it == h.end() ? std::string() : it->second;
}

} // namespace

bool isStreamingPayloadHash(std::string_view h)
{
    return h == "STREAMING-AWS4-HMAC-SHA256-PAYLOAD" ||
           h == "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER" ||
           h == "STREAMING-UNSIGNED-PAYLOAD-TRAILER";
}

PayloadResult preparePayload(std::string_view body, const PayloadOptions& opt)
{
    PayloadResult res;
    auto fail = [&](AuthError e) {
        res.error = std::move(e);
        res.payload.reset();
        return std::move(res);
    };
    const auto& h = *opt.headers;
    const std::string& ph = opt.payloadHash;
    const bool streaming = isStreamingPayloadHash(ph);
    const bool hex = isHex64(ph);
    if (!streaming && !hex && ph != "UNSIGNED-PAYLOAD")
        return fail(err(400, "InvalidRequest",
                        "Unsupported x-amz-content-sha256 value"));

    auto payload = std::make_shared<Payload>();

    // Content-Encoding without the aws-chunked token.
    {
        std::string ce = headerOr(h, "content-encoding"), out;
        size_t i = 0;
        while (i <= ce.size() && !ce.empty()) {
            size_t j = ce.find(',', i);
            if (j == std::string::npos)
                j = ce.size();
            auto tok = trimHeaderValue(ce.substr(i, j - i));
            if (!tok.empty() && upper(tok) != "AWS-CHUNKED")
                out += (out.empty() ? "" : ",") + tok;
            i = j + 1;
        }
        payload->contentEncoding = out;
    }

    // ---- what the client claims about the decoded bytes --------------------
    std::map<ChecksumAlgo, std::string> expected; // algo -> b64 (header form)
    for (auto& [name, val] : h)
        if (auto a = checksumAlgoFromHeader(name); a && opt.checkChecksumHeaders)
            expected[*a] = val;
    std::vector<ChecksumAlgo> trailerAlgos;
    if (ph == "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER" ||
        ph == "STREAMING-UNSIGNED-PAYLOAD-TRAILER") {
        std::string tr = headerOr(h, "x-amz-trailer");
        size_t i = 0;
        while (i <= tr.size() && !tr.empty()) {
            size_t j = tr.find(',', i);
            if (j == std::string::npos)
                j = tr.size();
            auto tok = trimHeaderValue(tr.substr(i, j - i));
            for (auto& c : tok)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (auto a = checksumAlgoFromHeader(tok))
                trailerAlgos.push_back(*a);
            i = j + 1;
        }
    }
    std::map<ChecksumAlgo, std::unique_ptr<Checksummer>> sums;
    for (auto& [a, _] : expected)
        sums[a] = std::make_unique<Checksummer>(a);
    for (auto a : trailerAlgos)
        if (!sums.count(a))
            sums[a] = std::make_unique<Checksummer>(a);

    std::string md5Want; // raw
    std::unique_ptr<Digest> md5;
    if (auto it = h.find("content-md5"); it != h.end()) {
        if (!base64Decode(trimHeaderValue(it->second), md5Want) ||
            md5Want.size() != 16)
            return fail(err(400, "InvalidDigest",
                            "The Content-MD5 you specified was invalid."));
        md5 = std::make_unique<Digest>(EVP_md5());
    }
    std::unique_ptr<Digest> sha;
    if (hex)
        sha = std::make_unique<Digest>(EVP_sha256());

    auto observe = [&](std::string_view d) {
        for (auto& [_, s] : sums)
            s->update(d);
        if (md5)
            md5->update(d);
        if (sha)
            sha->update(d);
    };

    // ---- the bytes ---------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> trailers;
    if (!streaming) {
        for (size_t off = 0; off < body.size(); off += kSlice)
            observe(body.substr(off, kSlice));
        payload->view = body;
    } else {
        auto it = h.find("x-amz-decoded-content-length");
        if (it == h.end())
            return fail(err(411, "MissingContentLength",
                            "You must provide the x-amz-decoded-content-length "
                            "header."));
        uint64_t declared = 0;
        bool ok = !it->second.empty() && it->second.size() <= 19;
        for (char c : it->second) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            declared = declared * 10 + static_cast<uint64_t>(c - '0');
        }
        if (!ok)
            return fail(err(400, "InvalidArgument",
                            "Invalid x-amz-decoded-content-length"));
        if (declared > opt.maxDecodedBytes)
            return fail(err(413, "EntityTooLarge",
                            "Your proposed upload exceeds the maximum allowed "
                            "size"));

        ChunkedDecoder::Params cp = opt.chunk;
        cp.signedChunks = ph.rfind("STREAMING-AWS4-HMAC-SHA256-PAYLOAD", 0) == 0;
        cp.trailer = ph.size() > 8 && ph.compare(ph.size() - 8, 8, "-TRAILER") == 0;
        ChunkedDecoder dec(cp);

        const bool inMemory = declared <= opt.memoryLimit;
        std::ofstream out;
        std::filesystem::path tmp;
        if (inMemory) {
            payload->memory.reserve(static_cast<size_t>(declared));
        } else {
            std::error_code ec;
            std::filesystem::create_directories(opt.tmpDir, ec);
            tmp = opt.tmpDir / ("decoded." + randomHex(12));
            out.open(tmp, std::ios::binary);
            if (!out)
                return fail(err(500, "InternalError", "Could not spool body"));
        }
        auto discard = [&] {
            if (!tmp.empty()) {
                out.close();
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
            }
        };
        uint64_t written = 0;
        auto sink = [&](std::string_view d) {
            if (written + d.size() > declared)
                throw std::length_error("decoded body longer than declared");
            written += d.size();
            observe(d);
            if (inMemory)
                payload->memory.append(d);
            else
                out.write(d.data(), static_cast<std::streamsize>(d.size()));
        };
        try {
            for (size_t off = 0; off < body.size(); off += kSlice) {
                auto st = dec.feed(body.substr(off, kSlice), sink);
                if (st == ChunkedDecoder::Status::BadSignature) {
                    discard();
                    return fail(err(403, "SignatureDoesNotMatch",
                                    "The request signature we calculated does "
                                    "not match the signature you provided."));
                }
                if (st != ChunkedDecoder::Status::Ok) {
                    discard();
                    return fail(err(400, "IncompleteBody",
                                    "The aws-chunked body is malformed: " +
                                        dec.error()));
                }
            }
        } catch (const std::length_error&) {
            discard();
            return fail(err(400, "IncompleteBody",
                            "The decoded body is longer than "
                            "x-amz-decoded-content-length."));
        } catch (...) {
            discard();
            return fail(err(500, "InternalError", "Could not decode body"));
        }
        if (!dec.done() || written != declared) {
            discard();
            return fail(err(400, "IncompleteBody",
                            "You did not provide the number of bytes specified "
                            "by the x-amz-decoded-content-length HTTP header."));
        }
        if (!inMemory) {
            out.close();
            if (!out) {
                discard();
                return fail(err(500, "InternalError", "Could not spool body"));
            }
            try {
                payload->file = std::make_unique<MappedFile>(tmp, true);
            } catch (...) {
                discard();
                return fail(err(500, "InternalError", "Could not map body"));
            }
            payload->view = payload->file->view();
        } else {
            payload->view = payload->memory;
        }
        trailers = dec.trailers();
    }

    // ---- verification ------------------------------------------------------
    if (sha) {
        auto want = ph;
        for (auto& c : want)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (hexEncode(sha->finish()) != want)
            return fail(err(400, "XAmzContentSHA256Mismatch",
                            "The provided 'x-amz-content-sha256' header does "
                            "not match what was computed."));
    }
    if (md5 && md5->finish() != md5Want)
        return fail(err(400, "BadDigest",
                        "The Content-MD5 you specified did not match what we "
                        "received."));
    std::map<ChecksumAlgo, std::string> computed;
    for (auto& [a, s] : sums)
        computed[a] = s->finish();
    auto mismatch = [&](ChecksumAlgo a) {
        return err(400, "BadDigest",
                   "The " + upper(std::string(checksumHeaderName(a)).substr(15)) +
                       " you specified did not match the calculated "
                       "checksum.");
    };
    for (auto& [a, want] : expected) {
        if (computed[a] != want)
            return fail(mismatch(a));
        if (!payload->checksum)
            payload->checksum = {checksumHeaderName(a), want};
    }
    for (auto a : trailerAlgos) {
        std::string got;
        for (auto& [name, val] : trailers)
            if (name == checksumHeaderName(a))
                got = val;
        if (got.empty())
            return fail(err(400, "InvalidRequest",
                            std::string("The trailing checksum ") +
                                checksumHeaderName(a) +
                                " was declared but not sent."));
        if (computed[a] != got)
            return fail(mismatch(a));
        if (!payload->checksum)
            payload->checksum = {checksumHeaderName(a), got};
    }
    res.payload = std::move(payload);
    return res;
}

} // namespace s3::sigv4
