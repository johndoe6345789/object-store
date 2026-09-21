/**
 * @file SecretCrypto.h
 * @brief AES-256-GCM encryption of api_keys secrets at rest.
 *
 * SigV4 needs the plaintext secret to derive the signing key, so secrets
 * cannot be hashed; the next best thing is to keep them encrypted under a
 * master key that lives in the environment, not in the database.
 *
 * Blob format (base64): 0x01 | 12-byte random nonce | ciphertext | 16-byte tag.
 * The access key is bound in as AAD, so a ciphertext copied onto another row
 * does not decrypt.
 */

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace s3
{

using MasterKey = std::array<unsigned char, 32>;

/// @brief 64 hex chars or base64 of 32 bytes -> key; nullopt if neither.
std::optional<MasterKey> parseMasterKey(std::string_view text);

std::string encryptSecret(const MasterKey& key, std::string_view plaintext,
                          std::string_view aad);

/// @brief nullopt for a wrong key, tampered blob or malformed input.
std::optional<std::string> decryptSecret(const MasterKey& key,
                                         std::string_view blob,
                                         std::string_view aad);

} // namespace s3
