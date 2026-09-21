/**
 * @file BucketOps.cpp
 * @brief ListBuckets, Create/Head/DeleteBucket, GetBucketLocation.
 */

#include "S3Ctx.h"

#include "../services/BucketStore.h"
#include "../services/Globals.h"
#include "../services/NameUtil.h"
#include "../services/ObjectCommit.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"
#include "../services/XmlUtil.h"

using namespace drogon;

namespace s3
{

namespace {
HttpResponsePtr noSuchBucket(const std::string& bucket)
{
    return s3Error(k404NotFound, "NoSuchBucket",
                   "The specified bucket does not exist", bucket);
}
} // namespace

void listBuckets(CtxPtr c, S3Callback&& cb)
{
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        auto buckets = BucketStore::list(c->owner);
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                          "<ListAllMyBucketsResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\"><Owner><ID>" + xmlEscape(c->owner) + "</ID><DisplayName>" +
               xmlEscape(c->owner) + "</DisplayName></Owner><Buckets>";
        for (const auto& b : buckets)
            xml += "<Bucket><Name>" + xmlEscape(b["name"].asString()) +
                   "</Name><CreationDate>" +
                   xmlEscape(b["created_at"].asString()) +
                   "</CreationDate></Bucket>";
        xml += "</Buckets></ListAllMyBucketsResult>";
        return xmlResponse(xml);
    });
}

void createBucket(CtxPtr c, S3Callback&& cb)
{
    if (!isValidBucketName(c->bucket)) {
        cb(s3Error(k400BadRequest, "InvalidBucketName",
                   "The specified bucket is not valid", c->bucket));
        return;
    }
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        // A bucket is always created for the authenticated key's own owner;
        // nothing in the request can name a different one.
        std::lock_guard<std::mutex> lock(blobCommitMutex());
        if (!BucketStore::create(c->bucket, Globals::region, c->owner)) {
            if (BucketStore::getId(c->bucket, c->owner) != 0)
                return s3Error(k409Conflict, "BucketAlreadyOwnedByYou",
                               "Your previous request to create the named "
                               "bucket succeeded and you already own it.",
                               c->bucket);
            return s3Error(k409Conflict, "BucketAlreadyExists",
                           "The requested bucket name is not available",
                           c->bucket);
        }
        // Blobs a previously deleted bucket of this name left behind belong
        // to nobody; never let a new owner's bucket start on top of them.
        Globals::blobs->removeBucket(c->bucket);
        auto r = HttpResponse::newHttpResponse();
        r->addHeader("Location", "/" + c->bucket);
        return r;
    });
}

void headBucket(CtxPtr c, S3Callback&& cb)
{
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        auto b = BucketStore::get(c->bucket, c->owner);
        if (b.isNull()) {
            auto r = HttpResponse::newHttpResponse();
            r->setStatusCode(k404NotFound);
            return r;
        }
        auto r = HttpResponse::newHttpResponse();
        r->addHeader("x-amz-bucket-region", b["region"].asString());
        return r;
    });
}

void deleteBucket(CtxPtr c, S3Callback&& cb)
{
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        std::lock_guard<std::mutex> lock(blobCommitMutex());
        int bid = BucketStore::getId(c->bucket, c->owner);
        if (bid == 0)
            return noSuchBucket(c->bucket);
        // S3 refuses to delete a bucket that still holds objects; silently
        // cascading would let one `aws s3 rb` destroy a whole tenant.
        if (ObjectStore::bucketHasObjects(bid))
            return s3Error(k409Conflict, "BucketNotEmpty",
                           "The bucket you tried to delete is not empty",
                           c->bucket);
        if (!BucketStore::remove(c->bucket, c->owner))
            return noSuchBucket(c->bucket);
        Globals::blobs->removeBucket(c->bucket);
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(k204NoContent);
        return r;
    });
}

void getBucketLocation(CtxPtr c, S3Callback&& cb)
{
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        auto b = BucketStore::get(c->bucket, c->owner);
        if (b.isNull())
            return noSuchBucket(c->bucket);
        return xmlResponse(
            std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<LocationConstraint xmlns=\"") +
            kS3Namespace + "\">" + xmlEscape(b["region"].asString()) +
            "</LocationConstraint>");
    });
}

} // namespace s3
