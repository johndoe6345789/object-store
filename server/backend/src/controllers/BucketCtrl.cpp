/**
 * @file BucketCtrl.cpp
 * @brief S3 bucket create, head, delete, list.
 */

#include "BucketCtrl.h"
#include "../services/BucketStore.h"
#include "../services/S3Response.h"
#include "../services/XmlUtil.h"
#include "../services/Globals.h"

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
    bool ok = BucketStore::create(bucket, Globals::region, requestOwner(req));
    if (!ok) {
        auto r = s3Error(k409Conflict, "BucketAlreadyExists",
                          "The requested bucket name is not available");
        cb(r);
        return;
    }
    auto r = HttpResponse::newHttpResponse();
    r->addHeader("Location", "/" + bucket);
    r->setStatusCode(k200OK);
    cb(r);
}

void BucketCtrl::headBucket(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& cb,
                            const std::string& bucket)
{
    auto b = BucketStore::get(bucket, requestOwner(req));
    auto r = HttpResponse::newHttpResponse();
    if (b.isNull()) {
        r->setStatusCode(k404NotFound);
    } else {
        r->addHeader("x-amz-bucket-region", b["region"].asString());
    }
    cb(r);
}

void BucketCtrl::deleteBucket(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& cb,
                              const std::string& bucket)
{
    bool ok = BucketStore::remove(bucket, requestOwner(req));
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(ok ? k204NoContent : k404NotFound);
    cb(r);
}

void BucketCtrl::listBuckets(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& cb)
{
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
    cb(r);
}

} // namespace s3
