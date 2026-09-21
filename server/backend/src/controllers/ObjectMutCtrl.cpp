/**
 * @file ObjectMutCtrl.cpp
 * @brief S3 object PUT + DELETE handlers.
 */

#include "../services/BucketStore.h"
#include "../services/NameUtil.h"
#include "../services/ObjectCommit.h"
#include "../services/S3Response.h"
#include "../services/Globals.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"
#include "MultipartCtrl.h"
#include "ObjectCtrl.h"

using namespace drogon;

namespace s3
{

void ObjectCtrl::putObject(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& cb,
                           const std::string& bucket, const std::string& key)
{
    // Multipart parts share this route; they are told apart by the query.
    if (queryValue(req->query(), "uploadId") ||
        queryValue(req->query(), "partNumber")) {
        MultipartCtrl::putPart(req, std::move(cb), bucket, key);
        return;
    }
    if (!isValidKey(key)) {
        cb(s3Error(k400BadRequest, "InvalidArgument", "Invalid object key"));
        return;
    }
    if (req->body().size() > Globals::maxObjectBytes) {
        cb(s3Error(k413RequestEntityTooLarge, "EntityTooLarge",
                   "Your proposed upload exceeds the maximum allowed size"));
        return;
    }
    auto ct = std::string(req->getHeader("Content-Type"));
    if (ct.empty())
        ct = "application/octet-stream";

    // `req` (shared_ptr) is captured so the body buffer outlives the handler:
    // stage() reads it via string_view (drogon keeps bodies over
    // setClientMaxMemoryBodySize in a mmapped temp file) and hashes/writes it
    // in 1 MiB chunks, so a large PUT is never copied into memory. The work runs on a Workers thread -- previously a
    // detached std::thread per request, which was unbounded, while the
    // bucket lookup still blocked the IO loop.
    offLoop(std::move(cb), [req, bucket, key, ct]() -> HttpResponsePtr {
        int bid = BucketStore::getId(
            bucket, req->attributes()->get<std::string>("owner"));
        if (bid == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist");

        auto staged = Globals::blobs->stage(bucket, req->body());
        auto etag = commitObject(bid, bucket, key, ct, staged);

        auto r = HttpResponse::newHttpResponse();
        r->addHeader("ETag", "\"" + etag + "\"");
        r->setStatusCode(k200OK);
        return r;
    });
}

void ObjectCtrl::deleteObject(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& cb,
                              const std::string& bucket, const std::string& key)
{
    if (queryValue(req->query(), "uploadId")) {
        MultipartCtrl::abort(req, std::move(cb), bucket, key);
        return;
    }
    offLoop(std::move(cb), [req, bucket, key]() -> HttpResponsePtr {
        int bid = BucketStore::getId(
            bucket, req->attributes()->get<std::string>("owner"));
        auto r = HttpResponse::newHttpResponse();
        if (bid == 0) {
            r->setStatusCode(k404NotFound);
            return r;
        }

        // Removes the blob only when no other key still points at it.
        commitDelete(bid, key);

        r->setStatusCode(k204NoContent);
        return r;
    });
}

} // namespace s3
