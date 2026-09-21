/**
 * @file Globals.h
 * @brief S3 server global state initialization.
 */

#pragma once

#include "BlobStore.h"
#include "DbPool.h"
#include "MultipartStore.h"
#include "SecretCrypto.h"
#include "../sigv4/SigV4Verify.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace s3
{

/// @brief Central service registry for the S3 server.
struct Globals {
    static inline std::unique_ptr<BlobStore> blobs;
    static inline std::unique_ptr<MultipartStore> uploads;
    static inline std::string region = "us-east-1";
    static inline std::filesystem::path dataDir = "/data/s3";
    /// Decoded aws-chunked bodies are spooled here (same filesystem as data).
    static inline std::filesystem::path tmpDir = "/data/s3/.body-tmp";
    /// SigV4 policy: region, S3_ANY_REGION, clock skew.
    static inline sigv4::VerifyConfig sigConfig;
    /// S3_SECRET_ENCRYPTION_KEY (and the optional _PREVIOUS for rotation).
    static inline std::optional<MasterKey> secretKey;
    static inline std::optional<MasterKey> previousSecretKey;
    /// Cap on one object (single PUT or completed multipart), S3_MAX_OBJECT_BYTES.
    static inline uintmax_t maxObjectBytes = 2ULL * 1024 * 1024 * 1024;
    /// Largest single body accepted for one multipart part.
    static constexpr uintmax_t kMaxPartBytes = 100ULL * 1024 * 1024;

    /// @brief Initialize all services.
    static void init(const std::filesystem::path& dataDir_,
                     const std::string& dbConn, const std::string& rgn)
    {
        namespace fs = std::filesystem;
        fs::create_directories(dataDir_);
        blobs = std::make_unique<BlobStore>(dataDir_ / "blobs");
        uploads = std::make_unique<MultipartStore>(dataDir_);
        region = rgn;
        dataDir = dataDir_;
        tmpDir = dataDir_ / ".body-tmp";
        fs::create_directories(tmpDir);
        sigConfig.region = rgn;
        DbPool::init(dbConn);
    }
};

} // namespace s3
