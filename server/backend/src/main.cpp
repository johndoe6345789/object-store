/**
 * @file main.cpp
 * @brief S3-compatible server entry point.
 */

#include "services/Globals.h"
#include "services/S3Response.h"
#include "services/Workers.h"
#include <drogon/drogon.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main(int argc, char* argv[])
{
    std::string dataDir = "/data/s3";
    std::string dbConn = "host=localhost port=5432 "
                         "dbname=s3server "
                         "user=s3user password=s3pass";
    std::string region = "us-east-1";
    int port = 9000;

    // Override from environment
    if (auto e = std::getenv("S3_DATA_DIR"))
        dataDir = e;
    if (auto e = std::getenv("S3_DB_CONN"))
        dbConn = e;
    if (auto e = std::getenv("S3_REGION"))
        region = e;
    if (auto e = std::getenv("S3_PORT"))
        port = std::stoi(e);

    if (auto e = std::getenv("S3_MAX_OBJECT_BYTES")) {
        try {
            s3::Globals::maxObjectBytes = std::stoull(e);
        } catch (...) {
            std::cerr << "ignoring invalid S3_MAX_OBJECT_BYTES\n";
        }
    }

    s3::Globals::init(dataDir, dbConn, region);

    // SigV4 policy.
    if (auto e = std::getenv("S3_ANY_REGION"))
        s3::Globals::sigConfig.anyRegion = std::string(e) == "1";
    if (auto e = std::getenv("S3_CLOCK_SKEW_SECONDS")) {
        try {
            s3::Globals::sigConfig.maxSkewSeconds = std::stol(e);
        } catch (...) {
            std::cerr << "ignoring invalid S3_CLOCK_SKEW_SECONDS\n";
        }
    }

    // At-rest encryption of api_keys secrets. A malformed key is fatal rather
    // than silently falling back to plaintext.
    if (auto e = std::getenv("S3_SECRET_ENCRYPTION_KEY"); e && *e) {
        s3::Globals::secretKey = s3::parseMasterKey(e);
        if (!s3::Globals::secretKey) {
            std::cerr << "[s3server] S3_SECRET_ENCRYPTION_KEY must be 64 hex "
                         "characters or base64 of 32 bytes\n";
            return 1;
        }
        if (auto p = std::getenv("S3_SECRET_ENCRYPTION_KEY_PREVIOUS");
            p && *p) {
            s3::Globals::previousSecretKey = s3::parseMasterKey(p);
            if (!s3::Globals::previousSecretKey) {
                std::cerr << "[s3server] S3_SECRET_ENCRYPTION_KEY_PREVIOUS is "
                             "not a valid key\n";
                return 1;
            }
        }
        std::cerr << "[s3server] api key secrets are encrypted at rest "
                     "(AES-256-GCM)\n";
    } else {
        std::cerr << "[s3server] WARNING: S3_SECRET_ENCRYPTION_KEY is not set; "
                     "api_keys secrets are stored in plaintext\n";
    }

    // Abandoned multipart uploads: sweep at start and hourly, on a plain
    // thread of their own so no IO loop or handler worker ever waits on it.
    std::thread([] {
        for (;;) {
            try {
                s3::Globals::uploads->sweep(std::chrono::hours(24));
                // Decoded aws-chunked bodies a crash left behind.
                namespace fs = std::filesystem;
                std::error_code ec;
                auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
                for (fs::directory_iterator it(s3::Globals::tmpDir, ec), end;
                     !ec && it != end; it.increment(ec))
                    if (it->path().filename().string().rfind("decoded.", 0) == 0 &&
                        fs::last_write_time(it->path(), ec) < cutoff)
                        fs::remove(it->path(), ec);
            } catch (...) {
            }
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }).detach();

    // Handlers run here, never on the IO loops: every store call is
    // synchronous, and a blocked loop stops answering every connection the
    // kernel gives it afterwards (services/Workers.h). Matched to the db
    // pool so a worker rarely waits for a connection.
    s3::Workers::init(8);

    drogon::app()
        .setLogPath("./")
        .setLogLevel(trantor::Logger::kInfo)
        .addListener("0.0.0.0", port)
        .setThreadNum(4)
        // Every response gets x-amz-request-id (and RequestId in errors).
        .registerPreSendingAdvice(s3::stampResponse)
        // XML bodies are sent as they are; SDKs sign/compare what they read.
        .enableGzip(false)
        .enableBrotli(false)
        // Body cap = the object cap (default 2 GiB; drogon's own default of
        // 1 MB rejects Docker layer pushes with HTTP 413), plus what
        // aws-chunked framing adds around a maximal object.
        .setClientMaxBodySize(s3::Globals::maxObjectBytes +
                              s3::Globals::maxObjectBytes / 32 + (1u << 20))
        // Bodies above 4 MiB are spooled by drogon to a temp file (mmapped,
        // not resident) instead of a std::string, so a big PUT or a 50-100 MB
        // part costs disk, not RAM. Same filesystem as the data dir.
        .setClientMaxMemoryBodySize(4 * 1024 * 1024)
        .setUploadPath(dataDir + "/.body-tmp")
        .run();

    return 0;
}
