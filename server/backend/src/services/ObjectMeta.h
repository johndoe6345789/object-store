/**
 * @file ObjectMeta.h
 * @brief System and user metadata (Cache-Control, Content-Disposition,
 *        x-amz-meta-*, ...) as stored in objects.metadata and echoed on
 *        GET/HEAD.
 */

#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <optional>
#include <string>

namespace s3
{

inline constexpr size_t kMaxUserMetadataBytes = 8192;

/// Headers stored verbatim and returned as sent.
inline const char* const kSystemMetaHeaders[] = {
    "cache-control", "content-disposition", "content-encoding",
    "content-language", "expires"};

/// @brief The metadata object for a new/replaced object. `contentEncoding`
///        overrides the request's (aws-chunked already stripped). Returns
///        nullopt when x-amz-meta-* exceeds kMaxUserMetadataBytes.
inline std::optional<Json::Value>
metadataFromRequest(const drogon::HttpRequestPtr& req,
                    const std::string& contentEncoding)
{
    Json::Value m(Json::objectValue);
    for (auto h : kSystemMetaHeaders) {
        const auto& v = req->getHeader(h);
        if (!v.empty())
            m[h] = v;
    }
    if (contentEncoding.empty())
        m.removeMember("content-encoding");
    else
        m["content-encoding"] = contentEncoding;
    size_t total = 0;
    for (const auto& [name, value] : req->headers()) {
        if (name.rfind("x-amz-meta-", 0) == 0) {
            total += name.size() + value.size();
            m[name] = value;
        }
    }
    if (total > kMaxUserMetadataBytes)
        return std::nullopt;
    return m;
}

/// @brief Set the stored metadata headers on a response.
inline void applyMetadata(const drogon::HttpResponsePtr& r, const Json::Value& m)
{
    if (!m.isObject())
        return;
    for (const auto& name : m.getMemberNames()) {
        if (name == "checksum" || !m[name].isString())
            continue;
        r->addHeader(name, m[name].asString());
    }
}

inline std::string toJsonString(const Json::Value& v)
{
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}

} // namespace s3
