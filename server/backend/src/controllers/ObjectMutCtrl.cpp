/**
 * @file ObjectMutCtrl.cpp
 * @brief S3 object PUT + DELETE handlers.
 */

#include "../services/BucketStore.h"
#include "../services/S3Response.h"
#include "../services/Globals.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"
#include "ObjectCtrl.h"

using namespace drogon;

namespace s3
{

void ObjectCtrl::putObject(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& cb,
                           const std::string& bucket, const std::string& key)
{
    auto ct = std::string(req->getHeader("Content-Type"));
    if (ct.empty())
        ct = "application/octet-stream";

    // `req` (shared_ptr) is captured so the body buffer outlives the handler:
    // store() reads it via string_view rather than copying what may be
    // hundreds of MB. The work runs on a Workers thread -- previously a
    // detached std::thread per request, which was unbounded, while the
    // bucket lookup still blocked the IO loop.
    offLoop(std::move(cb), [req, bucket, key, ct]() -> HttpResponsePtr {
        int bid = BucketStore::getId(
            bucket, req->attributes()->get<std::string>("owner"));
        if (bid == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist");

        auto res = Globals::blobs->store(bucket, key, req->body());
        ObjectStore::put(bid, key, res.etag, (int64_t)res.size, ct, res.path);

        auto r = HttpResponse::newHttpResponse();
        r->addHeader("ETag", "\"" + res.etag + "\"");
        r->setStatusCode(k200OK);
        return r;
    });
}

void ObjectCtrl::deleteObject(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& cb,
                              const std::string& bucket, const std::string& key)
{
    offLoop(std::move(cb), [req, bucket, key]() -> HttpResponsePtr {
        int bid = BucketStore::getId(
            bucket, req->attributes()->get<std::string>("owner"));
        auto r = HttpResponse::newHttpResponse();
        if (bid == 0) {
            r->setStatusCode(k404NotFound);
            return r;
        }

        auto path = ObjectStore::remove(bid, key);
        // Only drop the file when no other key still points at it: blobs are
        // content-addressed, so identical uploads share one file and deleting
        // it unconditionally emptied the survivors.
        if (!path.empty() && !ObjectStore::pathInUse(path))
            Globals::blobs->remove(path);

        r->setStatusCode(k204NoContent);
        return r;
    });
}

} // namespace s3
