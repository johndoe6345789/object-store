/**
 * @file ObjectCtrl.cpp
 * @brief S3 object GET + HEAD handlers.
 */

#include "ObjectCtrl.h"
#include "../services/BucketStore.h"
#include "../services/S3Response.h"
#include "../services/Globals.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"

using namespace drogon;

namespace s3
{

void ObjectCtrl::getObject(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& cb,
                           const std::string& bucket, const std::string& key)
{
    // The bucket lookup, the metadata query and the blob read all block; none
    // of them may run on the IO loop (see services/Workers.h).
    offLoop(std::move(cb), [req, bucket, key]() -> HttpResponsePtr {
        int bid = BucketStore::getId(
            bucket, req->attributes()->get<std::string>("owner"));
        if (bid == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist");

        auto meta = ObjectStore::get(bid, key);
        if (meta.isNull())
            return s3Error(k404NotFound, "NoSuchKey",
                           "The specified key does not exist");

        // Streamed from disk (sendfile for big blobs): a 1 GB object must
        // not be read into memory to be served.
        auto full = Globals::blobs->fullPath(meta["storage_path"].asString());
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec))
            // Metadata without bytes is a broken store, not an empty object:
            // say so instead of answering 200 with nothing.
            return s3Error(k500InternalServerError, "InternalError",
                           "The object data could not be read");

        auto r = HttpResponse::newFileResponse(
            full.string(), "", CT_NONE, meta["content_type"].asString());
        r->addHeader("ETag", "\"" + meta["etag"].asString() + "\"");
        // No manual Content-Length: the file response sets one, and the two
        // together are a duplicate header, which Node's fetch rejects as a
        // protocol violation.
        r->addHeader("Last-Modified", meta["last_modified"].asString());
        return r;
    });
}

void ObjectCtrl::headObject(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& cb,
                            const std::string& bucket, const std::string& key)
{
    offLoop(std::move(cb), [req, bucket, key]() -> HttpResponsePtr {
        int bid = BucketStore::getId(
            bucket, req->attributes()->get<std::string>("owner"));
        auto meta = (bid > 0) ? ObjectStore::get(bid, key) : Json::nullValue;
        auto r = HttpResponse::newHttpResponse();
        if (meta.isNull()) {
            r->setStatusCode(k404NotFound);
            return r;
        }
        r->addHeader("ETag", "\"" + meta["etag"].asString() + "\"");
        // HEAD has no body for setBody to measure, so the size has to be
        // stated. If drogon also emits a zero-length header here this has the
        // same duplicate problem as GET did; untested, since nothing calls
        // HEAD yet.
        r->addHeader("Content-Length", std::to_string(meta["size"].asInt64()));
        r->setContentTypeString(meta["content_type"].asString());
        return r;
    });
}

} // namespace s3
