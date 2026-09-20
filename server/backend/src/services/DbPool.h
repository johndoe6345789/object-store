/**
 * @file DbPool.h
 * @brief PostgreSQL connection pool for S3 server.
 */

#pragma once

#include <drogon/orm/DbClient.h>

#include <string>

namespace s3
{

/// @brief Singleton database connection pool.
class DbPool
{
  public:
    /// @brief Initialize with a PG connection string.
    static void init(const std::string& connStr)
    {
        // A query that never returns holds a worker forever, so bound it.
        // Only added when the caller has not set libpq options itself.
        auto conn = connStr;
        if (conn.find("options") == std::string::npos)
            conn += " options='-c statement_timeout=15000'";
        // One connection per worker thread (Workers::init), so a handler
        // does not queue behind another's query.
        client_ = drogon::orm::DbClient::newPgClient(conn, 8);
    }

    /// @brief Get the shared DbClient.
    static drogon::orm::DbClientPtr& get() { return client_; }

  private:
    static inline drogon::orm::DbClientPtr client_;
};

} // namespace s3
