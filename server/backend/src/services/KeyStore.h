/**
 * @file KeyStore.h
 * @brief api_keys lookup with optional at-rest encryption of secrets.
 */

#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace s3
{

struct KeyRecord {
    std::string secret; ///< plaintext, for deriving the SigV4 signing key
    std::string owner;
    std::string permissions;
};

/// Thrown when a row cannot be used (encrypted but no/wrong master key).
struct KeyStoreError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class KeyStore
{
  public:
    /// Marker left in api_keys.secret_key once the secret lives in secret_enc.
    static constexpr const char* kEncryptedMarker = "!enc";

    /// @brief Blocking (DB): the record for an access key, or nullopt.
    ///
    /// With a master key configured, a plaintext secret is encrypted on first
    /// use (secret_enc set, secret_key replaced by the marker). A plaintext
    /// secret_key always wins over secret_enc, so rotating a secret is just
    /// `UPDATE api_keys SET secret_key='new'`; it is re-encrypted next use.
    static std::optional<KeyRecord> lookup(const std::string& accessKey);
};

} // namespace s3
