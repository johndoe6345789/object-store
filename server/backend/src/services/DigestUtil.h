/**
 * @file DigestUtil.h
 * @brief MD5 digest helpers using OpenSSL 3.x EVP API, and random hex ids.
 */

#pragma once

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace s3
{

/// @brief Incremental MD5, so large blobs are hashed as they are written
///        rather than held in memory.
class Md5
{
  public:
    Md5() : ctx_(EVP_MD_CTX_new())
    {
        EVP_DigestInit_ex(ctx_, EVP_md5(), nullptr);
    }
    ~Md5() { EVP_MD_CTX_free(ctx_); }
    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    void update(std::string_view data)
    {
        EVP_DigestUpdate(ctx_, data.data(), data.size());
    }

    /// @brief Finish and return the lowercase hex digest (call once).
    std::string hex()
    {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_DigestFinal_ex(ctx_, hash, &len);
        std::ostringstream ss;
        for (unsigned int i = 0; i < len; ++i)
            ss << std::hex << std::setfill('0') << std::setw(2)
               << (int)hash[i];
        return ss.str();
    }

  private:
    EVP_MD_CTX* ctx_;
};

/// @brief Compute MD5 hex digest of data (accepts string_view to avoid copies).
inline std::string md5hex(std::string_view data)
{
    Md5 m;
    m.update(data);
    return m.hex();
}

/// @brief `nBytes` of CSPRNG output as lowercase hex (2*nBytes chars).
inline std::string randomHex(size_t nBytes)
{
    std::string raw(nBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()),
                   static_cast<int>(nBytes)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(nBytes * 2);
    for (unsigned char c : raw) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

} // namespace s3
