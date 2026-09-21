/**
 * @file S3Ctx.h
 * @brief What every S3 operation handler gets: the decoded bucket/key, the
 *        parsed query, the authenticated owner and the verified body.
 */

#pragma once

#include "../services/AuthUtil.h"
#include "../services/S3Response.h"
#include "../sigv4/PayloadReader.h"
#include "../sigv4/UriEncoding.h"

#include <drogon/HttpRequest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace s3
{

using S3Callback = std::function<void(const drogon::HttpResponsePtr&)>;

/// @brief Query string, percent-decoded ('+' is a space here, not in paths).
class Query
{
  public:
    static Query parse(std::string_view raw)
    {
        Query q;
        size_t i = 0;
        while (i <= raw.size() && !raw.empty()) {
            size_t j = raw.find('&', i);
            if (j == std::string_view::npos)
                j = raw.size();
            auto pair = raw.substr(i, j - i);
            i = j + 1;
            if (pair.empty())
                continue;
            auto eq = pair.find('=');
            q.items_.emplace_back(
                sigv4::uriDecode(pair.substr(0, eq), true),
                eq == std::string_view::npos
                    ? std::string()
                    : sigv4::uriDecode(pair.substr(eq + 1), true));
        }
        return q;
    }
    bool has(std::string_view n) const { return find(n).has_value(); }
    std::optional<std::string> find(std::string_view n) const
    {
        for (auto& [k, v] : items_)
            if (k == n)
                return v;
        return std::nullopt;
    }
    std::string get(std::string_view n, std::string def = "") const
    {
        auto v = find(n);
        return v ? *v : def;
    }
    const std::vector<std::pair<std::string, std::string>>& items() const
    {
        return items_;
    }

  private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct S3Ctx {
    drogon::HttpRequestPtr req;
    Query query;
    std::string bucket;
    std::string key; ///< empty for bucket-level operations
    std::string owner;
    std::string permissions;

    /// The decoded, verified body (aws-chunked already unwrapped).
    std::shared_ptr<sigv4::Payload> payload;
    std::string_view body() const
    {
        return payload ? payload->view : std::string_view(req->body());
    }
    bool head() const { return req->isHead(); }
    bool mayRead() const { return isAllowed(permissions, true); }
    std::string header(const char* n) const { return req->getHeader(n); }
};
using CtxPtr = std::shared_ptr<S3Ctx>;

// ---- operations (one .cpp per concern) --------------------------------
void listBuckets(CtxPtr, S3Callback&&);
void createBucket(CtxPtr, S3Callback&&);
void headBucket(CtxPtr, S3Callback&&);
void deleteBucket(CtxPtr, S3Callback&&);
void getBucketLocation(CtxPtr, S3Callback&&);

void listObjects(CtxPtr, S3Callback&&);

void getObject(CtxPtr, S3Callback&&);
void putObject(CtxPtr, S3Callback&&);
void copyObject(CtxPtr, S3Callback&&);
void deleteObject(CtxPtr, S3Callback&&);
void deleteObjects(CtxPtr, S3Callback&&);

void initiateMultipart(CtxPtr, S3Callback&&);
void uploadPart(CtxPtr, S3Callback&&);
void uploadPartCopy(CtxPtr, S3Callback&&);
void completeMultipart(CtxPtr, S3Callback&&);
void abortMultipart(CtxPtr, S3Callback&&);
void listParts(CtxPtr, S3Callback&&);
void listMultipartUploads(CtxPtr, S3Callback&&);

/// XML 200 response helper.
inline drogon::HttpResponsePtr xmlResponse(const std::string& xml)
{
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setContentTypeString("application/xml");
    r->setBody(xml);
    return r;
}

} // namespace s3
