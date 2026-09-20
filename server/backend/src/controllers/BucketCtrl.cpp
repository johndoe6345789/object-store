/**
 * @file BucketCtrl.cpp
 * @brief S3 bucket create, head, delete, list.
 */

#include "BucketCtrl.h"
#include "../services/BucketStore.h"
#include "../services/S3Response.h"
#include "../services/XmlUtil.h"
#include "../services/Globals.h"
#include "../services/OffLoop.h"

using namespace drogon;

namespace s3
{

namespace {
std::string requestOwner(const HttpRequestPtr& req)
{
    return req->attributes()->get<std::string>("owner");
}
}

void BucketCtrl::createBucket(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& cb,
                              const std::string& bucket)
{
    // Every handler here blocks in postgres, so none may run on an IO loop
    // (services/Workers.h).
    offLoop(std::move(cb), [req, bucket]() -> HttpResponsePtr {
        if (!BucketStore::create(bucket, Globals::region, requestOwner(req)))
            return s3Error(k409Conflict, "BucketAlreadyExists",
                           "The requested bucket name is not available");
        auto r = HttpResponse::newHttpResponse();
        r->addHeader("Location", "/" + bucket);
        r->setStatusCode(k200OK);
        return r;
    });
}

void BucketCtrl::headBucket(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& cb,
                            const std::string& bucket)
{
    offLoop(std::move(cb), [req, bucket]() -> HttpResponsePtr {
        auto b = BucketStore::get(bucket, requestOwner(req));
        auto r = HttpResponse::newHttpResponse();
        if (b.isNull())
            r->setStatusCode(k404NotFound);
        else
            r->addHeader("x-amz-bucket-region", b["region"].asString());
        return r;
    });
}

void BucketCtrl::deleteBucket(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& cb,
                              const std::string& bucket)
{
    offLoop(std::move(cb), [req, bucket]() -> HttpResponsePtr {
        bool ok = BucketStore::remove(bucket, requestOwner(req));
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(ok ? k204NoContent : k404NotFound);
        return r;
    });
}

void BucketCtrl::listBuckets(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& cb)
{
    offLoop(std::move(cb), [req]() -> HttpResponsePtr {
        auto buckets = BucketStore::list(requestOwner(req));
        // Return S3-style XML response
        std::string xml = "<?xml version=\"1.0\"?>"
                          "<ListAllMyBucketsResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\"><Buckets>";
        for (const auto& b : buckets) {
            // Names are escaped and the date converted: PostgreSQL's format is
            // not the ISO8601 an S3 client parses into a timestamp.
            xml += "<Bucket><Name>" + xmlEscape(b["name"].asString()) +
                   "</Name><CreationDate>" +
                   xmlEscape(isoTimestamp(b["created_at"].asString())) +
                   "</CreationDate></Bucket>";
        }
        xml += "</Buckets></ListAllMyBucketsResult>";
        auto r = HttpResponse::newHttpResponse();
        r->setContentTypeString("application/xml");
        r->setBody(xml);
        return r;
    });
}

} // namespace s3
