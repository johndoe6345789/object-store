/**
 * @file main.cpp
 * @brief S3-compatible server entry point.
 */

#include "services/Globals.h"
#include "services/Workers.h"
#include <drogon/drogon.h>
#include <chrono>
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

    // Abandoned multipart uploads: sweep at start and hourly, on a plain
    // thread of their own so no IO loop or handler worker ever waits on it.
    std::thread([] {
        for (;;) {
            try {
                s3::Globals::uploads->sweep(std::chrono::hours(24));
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
        // Body cap = the object cap (default 2 GiB; drogon's own default of
        // 1 MB rejects Docker layer pushes with HTTP 413).
        .setClientMaxBodySize(s3::Globals::maxObjectBytes)
        // Bodies above 4 MiB are spooled by drogon to a temp file (mmapped,
        // not resident) instead of a std::string, so a big PUT or a 50-100 MB
        // part costs disk, not RAM. Same filesystem as the data dir.
        .setClientMaxMemoryBodySize(4 * 1024 * 1024)
        .setUploadPath(dataDir + "/.body-tmp")
        .run();

    return 0;
}
