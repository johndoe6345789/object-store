/**
 * @file Globals.h
 * @brief S3 server global state initialization.
 */

#pragma once

#include "BlobStore.h"
#include "DbPool.h"
#include "MultipartStore.h"

#include <filesystem>
#include <memory>
#include <string>

namespace s3
{

/// @brief Central service registry for the S3 server.
struct Globals {
    static inline std::unique_ptr<BlobStore> blobs;
    static inline std::unique_ptr<MultipartStore> uploads;
    static inline std::string region = "us-east-1";
    /// Cap on one object (single PUT or completed multipart), S3_MAX_OBJECT_BYTES.
    static inline uintmax_t maxObjectBytes = 2ULL * 1024 * 1024 * 1024;
    /// Largest single body accepted for one multipart part.
    static constexpr uintmax_t kMaxPartBytes = 100ULL * 1024 * 1024;

    /// @brief Initialize all services.
    static void init(const std::filesystem::path& dataDir,
                     const std::string& dbConn, const std::string& rgn)
    {
        namespace fs = std::filesystem;
        fs::create_directories(dataDir);
        blobs = std::make_unique<BlobStore>(dataDir / "blobs");
        uploads = std::make_unique<MultipartStore>(dataDir);
        region = rgn;
        DbPool::init(dbConn);
    }
};

} // namespace s3
