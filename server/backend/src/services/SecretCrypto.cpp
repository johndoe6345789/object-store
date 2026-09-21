/**
 * @file SecretCrypto.cpp
 * @brief See SecretCrypto.h.
 */

#include "SecretCrypto.h"

#include "../sigv4/Crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

namespace s3
{

namespace {
constexpr unsigned char kVersion = 1;
constexpr size_t kNonce = 12, kTag = 16;

struct CtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
using Ctx = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;
} // namespace

std::optional<MasterKey> parseMasterKey(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' ||
                             text.back() == '\r'))
        text.remove_suffix(1);
    std::string raw;
    if (text.size() == 64 && sigv4::hexDecode(text, raw) && raw.size() == 32) {
    } else if (!sigv4::base64Decode(text, raw) || raw.size() != 32) {
        return std::nullopt;
    }
    MasterKey k;
    std::copy(raw.begin(), raw.end(), k.begin());
    return k;
}

std::string encryptSecret(const MasterKey& key, std::string_view plaintext,
                          std::string_view aad)
{
    unsigned char nonce[kNonce];
    if (RAND_bytes(nonce, sizeof nonce) != 1)
        throw std::runtime_error("RAND_bytes failed");
    Ctx ctx(EVP_CIPHER_CTX_new());
    std::string ct(plaintext.size(), '\0');
    unsigned char tag[kTag];
    int len = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(),
                           nonce) == 1 &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) == 1 &&
        EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(ct.data()),
                          &len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) == 1 &&
        EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(ct.data()) + len,
                            &len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTag, tag) == 1;
    if (!ok)
        throw std::runtime_error("encryption failed");
    std::string blob(1, static_cast<char>(kVersion));
    blob.append(reinterpret_cast<char*>(nonce), kNonce);
    blob += ct;
    blob.append(reinterpret_cast<char*>(tag), kTag);
    return sigv4::base64Encode(blob);
}

std::optional<std::string> decryptSecret(const MasterKey& key,
                                         std::string_view b64,
                                         std::string_view aad)
{
    std::string blob;
    if (!sigv4::base64Decode(b64, blob) || blob.size() < 1 + kNonce + kTag ||
        static_cast<unsigned char>(blob[0]) != kVersion)
        return std::nullopt;
    const auto* nonce = reinterpret_cast<const unsigned char*>(blob.data() + 1);
    const size_t ctLen = blob.size() - 1 - kNonce - kTag;
    const auto* ctp = reinterpret_cast<const unsigned char*>(blob.data() + 1 + kNonce);
    unsigned char tag[kTag];
    std::copy(blob.end() - kTag, blob.end(), tag);
    Ctx ctx(EVP_CIPHER_CTX_new());
    std::string pt(ctLen, '\0');
    int len = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(),
                           nonce) != 1 ||
        EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) != 1 ||
        EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(pt.data()),
                          &len, ctp, static_cast<int>(ctLen)) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTag, tag) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(),
                            reinterpret_cast<unsigned char*>(pt.data()) + len,
                            &len) != 1)
        return std::nullopt;
    return pt;
}

} // namespace s3
