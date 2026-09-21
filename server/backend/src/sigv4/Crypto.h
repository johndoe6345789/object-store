/**
 * @file Crypto.h
 * @brief Small OpenSSL wrappers for SigV4: SHA-256, HMAC-SHA256, hex/base64.
 */

#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace s3::sigv4
{

inline std::string hexEncode(std::string_view raw)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

/// @brief Decode lowercase/uppercase hex; nullopt-like empty + ok flag.
inline bool hexDecode(std::string_view hex, std::string& out)
{
    if (hex.size() % 2)
        return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        int a = val(hex[i]), b = val(hex[i + 1]);
        if (a < 0 || b < 0)
            return false;
        out += static_cast<char>(a * 16 + b);
    }
    return true;
}

inline std::string base64Encode(std::string_view raw)
{
    std::string out(4 * ((raw.size() + 2) / 3), '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(raw.data()),
                            static_cast<int>(raw.size()));
    out.resize(static_cast<size_t>(n));
    return out;
}

/// @brief Strict base64 (padding required, no whitespace). False if invalid.
inline bool base64Decode(std::string_view in, std::string& out)
{
    if (in.empty() || in.size() % 4)
        return false;
    for (char c : in)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            return false;
    std::string buf(in.size() / 4 * 3, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(buf.data()),
                            reinterpret_cast<const unsigned char*>(in.data()),
                            static_cast<int>(in.size()));
    if (n < 0)
        return false;
    size_t pad = in.back() == '=' ? (in[in.size() - 2] == '=' ? 2 : 1) : 0;
    buf.resize(static_cast<size_t>(n) - pad);
    out = std::move(buf);
    return true;
}

/// @brief Incremental digest (sha256/sha1/md5) returning raw bytes.
class Digest
{
  public:
    explicit Digest(const EVP_MD* md) : ctx_(EVP_MD_CTX_new())
    {
        if (!ctx_ || EVP_DigestInit_ex(ctx_, md, nullptr) != 1)
            throw std::runtime_error("digest init failed");
    }
    ~Digest() { EVP_MD_CTX_free(ctx_); }
    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;

    void update(std::string_view d)
    {
        EVP_DigestUpdate(ctx_, d.data(), d.size());
    }
    std::string finish()
    {
        unsigned char h[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_DigestFinal_ex(ctx_, h, &len);
        return std::string(reinterpret_cast<char*>(h), len);
    }
    /// @brief Copy of the running state (for chunk hashing without reset).
    void reset(const EVP_MD* md) { EVP_DigestInit_ex(ctx_, md, nullptr); }

  private:
    EVP_MD_CTX* ctx_;
};

inline std::string sha256Raw(std::string_view d)
{
    Digest h(EVP_sha256());
    h.update(d);
    return h.finish();
}
inline std::string sha256Hex(std::string_view d)
{
    return hexEncode(sha256Raw(d));
}

inline std::string hmacSha256(std::string_view key, std::string_view data)
{
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), out,
         &len);
    return std::string(reinterpret_cast<char*>(out), len);
}

} // namespace s3::sigv4
