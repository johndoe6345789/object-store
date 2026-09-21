/**
 * @file Checksum.h
 * @brief The additional-checksum algorithms S3 SDKs send: CRC32, CRC32C,
 *        CRC64NVME, SHA1, SHA256 -- incremental, base64 result.
 */

#pragma once

#include "Crypto.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace s3::sigv4
{

enum class ChecksumAlgo { Crc32, Crc32c, Crc64Nvme, Sha1, Sha256 };

/// @brief "crc32", "CRC32C", ... (case-insensitive) -> algorithm.
inline std::optional<ChecksumAlgo> checksumAlgoFromName(std::string_view n)
{
    std::string l(n);
    for (auto& c : l)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "crc32")
        return ChecksumAlgo::Crc32;
    if (l == "crc32c")
        return ChecksumAlgo::Crc32c;
    if (l == "crc64nvme")
        return ChecksumAlgo::Crc64Nvme;
    if (l == "sha1")
        return ChecksumAlgo::Sha1;
    if (l == "sha256")
        return ChecksumAlgo::Sha256;
    return std::nullopt;
}

inline const char* checksumHeaderName(ChecksumAlgo a)
{
    switch (a) {
    case ChecksumAlgo::Crc32: return "x-amz-checksum-crc32";
    case ChecksumAlgo::Crc32c: return "x-amz-checksum-crc32c";
    case ChecksumAlgo::Crc64Nvme: return "x-amz-checksum-crc64nvme";
    case ChecksumAlgo::Sha1: return "x-amz-checksum-sha1";
    case ChecksumAlgo::Sha256: return "x-amz-checksum-sha256";
    }
    return "";
}

/// @brief "x-amz-checksum-crc32" -> Crc32; nullopt for anything else
///        (including x-amz-checksum-mode / -type).
inline std::optional<ChecksumAlgo> checksumAlgoFromHeader(std::string_view h)
{
    constexpr std::string_view p = "x-amz-checksum-";
    if (h.substr(0, p.size()) != p)
        return std::nullopt;
    return checksumAlgoFromName(h.substr(p.size()));
}

/// @brief Reflected, table-driven CRC with init/xorout of all ones.
template <typename T>
class Crc
{
  public:
    explicit Crc(T poly)
    {
        for (unsigned i = 0; i < 256; ++i) {
            T c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
            table_[i] = c;
        }
    }
    T update(T crc, std::string_view d) const
    {
        for (unsigned char b : d)
            crc = table_[(crc ^ b) & 0xff] ^ (crc >> 8);
        return crc;
    }

  private:
    std::array<T, 256> table_;
};

/// @brief One running checksum; finish() gives the base64 the wire uses.
class Checksummer
{
  public:
    explicit Checksummer(ChecksumAlgo a) : algo_(a)
    {
        switch (a) {
        case ChecksumAlgo::Sha1:
            sha_ = std::make_unique<Digest>(EVP_sha1());
            break;
        case ChecksumAlgo::Sha256:
            sha_ = std::make_unique<Digest>(EVP_sha256());
            break;
        case ChecksumAlgo::Crc32:
            crc32_ = ~uint32_t(0);
            break;
        case ChecksumAlgo::Crc32c:
            crc32_ = ~uint32_t(0);
            break;
        case ChecksumAlgo::Crc64Nvme:
            crc64_ = ~uint64_t(0);
            break;
        }
    }
    ChecksumAlgo algo() const { return algo_; }

    void update(std::string_view d)
    {
        switch (algo_) {
        case ChecksumAlgo::Sha1:
        case ChecksumAlgo::Sha256:
            sha_->update(d);
            break;
        case ChecksumAlgo::Crc32:
            crc32_ = table32().update(crc32_, d);
            break;
        case ChecksumAlgo::Crc32c:
            crc32_ = table32c().update(crc32_, d);
            break;
        case ChecksumAlgo::Crc64Nvme:
            crc64_ = table64().update(crc64_, d);
            break;
        }
    }

    std::string finish()
    {
        std::string raw;
        auto be = [&](uint64_t v, int bytes) {
            for (int i = bytes - 1; i >= 0; --i)
                raw += static_cast<char>((v >> (8 * i)) & 0xff);
        };
        switch (algo_) {
        case ChecksumAlgo::Sha1:
        case ChecksumAlgo::Sha256:
            raw = sha_->finish();
            break;
        case ChecksumAlgo::Crc32:
        case ChecksumAlgo::Crc32c:
            be(~crc32_, 4);
            break;
        case ChecksumAlgo::Crc64Nvme:
            be(~crc64_, 8);
            break;
        }
        return base64Encode(raw);
    }

  private:
    static const Crc<uint32_t>& table32()
    {
        static const Crc<uint32_t> t(0xEDB88320u);
        return t;
    }
    static const Crc<uint32_t>& table32c()
    {
        static const Crc<uint32_t> t(0x82F63B78u);
        return t;
    }
    static const Crc<uint64_t>& table64()
    {
        static const Crc<uint64_t> t(0x9A6C9329AC4BC9B5ull);
        return t;
    }

    ChecksumAlgo algo_;
    std::unique_ptr<Digest> sha_;
    uint32_t crc32_ = 0;
    uint64_t crc64_ = 0;
};

} // namespace s3::sigv4
