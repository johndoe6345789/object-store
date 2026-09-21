/**
 * @file ObjectStoreUtil.h
 * @brief Shared helpers for ObjectStore row mapping.
 */

#pragma once

#include <drogon/orm/Row.h>
#include <json/json.h>

namespace s3
{

/// @brief Row-to-JSON helpers for object metadata.
struct ObjectStoreUtil {
    /// Column list for every object SELECT: the row plus its modification time
    /// as UTC ISO8601 (ms) and epoch seconds, so nothing depends on the
    /// server's TimeZone setting or on parsing PostgreSQL's text format.
    static constexpr const char* kCols =
        "*, to_char(updated_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS updated_iso, "
        "extract(epoch from updated_at)::bigint AS updated_epoch";

    /// @brief Convert a DB row to Json::Value.
    static Json::Value rowToJson(const drogon::orm::Row& row)
    {
        Json::Value j;
        j["key"] = row["key"].as<std::string>();
        j["etag"] = row["etag"].as<std::string>();
        j["size"] = (Json::Int64)row["size"].as<int64_t>();
        j["content_type"] = row["content_type"].as<std::string>();
        j["storage_path"] = row["storage_path"].as<std::string>();
        j["last_modified"] = row["updated_iso"].as<std::string>();
        j["last_modified_epoch"] = (Json::Int64)row["updated_epoch"].as<int64_t>();
        Json::Value md;
        Json::Reader().parse(row["metadata"].as<std::string>(), md);
        j["metadata"] = md.isObject() ? md : Json::Value(Json::objectValue);
        return j;
    }
};

} // namespace s3
