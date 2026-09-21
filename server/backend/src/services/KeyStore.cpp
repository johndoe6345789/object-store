/**
 * @file KeyStore.cpp
 * @brief See KeyStore.h.
 */

#include "KeyStore.h"

#include "DbPool.h"
#include "Globals.h"
#include "SecretCrypto.h"

#include <iostream>

namespace s3
{

std::optional<KeyRecord> KeyStore::lookup(const std::string& accessKey)
{
    auto rows = DbPool::get()->execSqlSync(
        "SELECT secret_key, secret_enc, owner, permissions "
        "FROM api_keys WHERE access_key=$1",
        accessKey);
    if (rows.empty())
        return std::nullopt;
    const auto& row = rows[0];
    KeyRecord rec;
    rec.owner = row["owner"].as<std::string>();
    rec.permissions = row["permissions"].as<std::string>();
    const auto stored = row["secret_key"].as<std::string>();
    const bool hasEnc = !row["secret_enc"].isNull();

    if (hasEnc && stored == kEncryptedMarker) {
        const auto blob = row["secret_enc"].as<std::string>();
        if (!Globals::secretKey)
            throw KeyStoreError("api key is encrypted but "
                                "S3_SECRET_ENCRYPTION_KEY is not set");
        auto plain = decryptSecret(*Globals::secretKey, blob, accessKey);
        if (!plain && Globals::previousSecretKey) {
            plain = decryptSecret(*Globals::previousSecretKey, blob, accessKey);
            if (plain)
                try { // migrate to the current master key
                    DbPool::get()->execSqlSync(
                        "UPDATE api_keys SET secret_enc=$1 WHERE access_key=$2 "
                        "AND secret_enc=$3",
                        encryptSecret(*Globals::secretKey, *plain, accessKey),
                        accessKey, blob);
                } catch (...) {
                }
        }
        if (!plain)
            throw KeyStoreError("api key cannot be decrypted with the "
                                "configured master key");
        rec.secret = std::move(*plain);
        return rec;
    }

    rec.secret = stored;
    if (Globals::secretKey) {
        // Lazy re-encryption. Guarded on the plaintext still being what we
        // read, so a concurrent rotation is never overwritten.
        try {
            DbPool::get()->execSqlSync(
                "UPDATE api_keys SET secret_enc=$1, secret_key=$2 "
                "WHERE access_key=$3 AND secret_key=$4",
                encryptSecret(*Globals::secretKey, stored, accessKey),
                std::string(kEncryptedMarker), accessKey, stored);
        } catch (const std::exception& e) {
            std::cerr << "[s3server] could not encrypt secret of one key: "
                      << e.what() << "\n";
        }
    }
    return rec;
}

} // namespace s3
