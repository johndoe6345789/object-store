/**
 * @file MultipartCtrl.cpp
 * @brief Multipart upload handlers. All work runs off the IO loops.
 */

#include "MultipartCtrl.h"
#include "../services/BucketStore.h"
#include "../services/Globals.h"
#include "../services/NameUtil.h"
#include "../services/ObjectCommit.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"

using namespace drogon;

namespace s3
{

namespace {

HttpStatusCode statusFor(const std::string& code)
{
    if (code == "EntityTooLarge")
        return k413RequestEntityTooLarge;
    if (code == "InternalError")
        return k500InternalServerError;
    return k400BadRequest;
}

/// Auth + ownership for every multipart route: the bucket must belong to the
/// authenticated owner, and the upload must have been initiated by that same
/// (owner, bucket, key). A foreign or unknown upload is NoSuchUpload either
/// way, so ids of other owners' uploads are not confirmable.
struct Target {
    HttpResponsePtr error;
    int bucketId = 0;
    MultipartStore::Meta meta;
};

Target resolve(const HttpRequestPtr& req, const std::string& bucket,
               const std::string& key, const std::string& uploadId)
{
    Target t;
    const auto owner = req->attributes()->get<std::string>("owner");
    t.bucketId = BucketStore::getId(bucket, owner);
    if (t.bucketId == 0) {
        t.error = s3Error(k404NotFound, "NoSuchBucket",
                          "The specified bucket does not exist");
        return t;
    }
    auto m = Globals::uploads->load(uploadId);
    if (!m || m->owner != owner || m->bucket != bucket || m->key != key) {
        t.error = s3Error(k404NotFound, "NoSuchUpload",
                          "The specified upload does not exist");
        return t;
    }
    t.meta = *m;
    return t;
}

HttpResponsePtr busy()
{
    return s3Error(k409Conflict, "OperationAborted",
                   "A conflicting operation on this upload is in progress");
}

} // namespace

void MultipartCtrl::post(const HttpRequestPtr& req, S3Callback&& cb,
                         const std::string& bucket, const std::string& key)
{
    const auto uploadId = queryValue(req->query(), "uploadId");
    const bool initiate = queryValue(req->query(), "uploads").has_value();
    if (!isValidKey(key)) {
        cb(s3Error(k400BadRequest, "InvalidArgument", "Invalid object key"));
        return;
    }
    if (initiate == uploadId.has_value()) {
        cb(s3Error(k501NotImplemented, "NotImplemented",
                   "Use ?uploads or ?uploadId=ID"));
        return;
    }
    auto ct = std::string(req->getHeader("Content-Type"));
    if (ct.empty())
        ct = "application/octet-stream";

    if (initiate) {
        offLoop(std::move(cb), [req, bucket, key, ct]() -> HttpResponsePtr {
            const auto owner = req->attributes()->get<std::string>("owner");
            if (BucketStore::getId(bucket, owner) == 0)
                return s3Error(k404NotFound, "NoSuchBucket",
                               "The specified bucket does not exist");
            auto id = Globals::uploads->create({owner, bucket, key, ct});
            std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<InitiateMultipartUploadResult xmlns=\"";
            xml += kS3Namespace;
            xml += "\"><Bucket>" + xmlEscape(bucket) + "</Bucket><Key>" +
                   xmlEscape(key) + "</Key><UploadId>" + id +
                   "</UploadId></InitiateMultipartUploadResult>";
            auto r = HttpResponse::newHttpResponse();
            r->setContentTypeString("application/xml");
            r->setBody(xml);
            return r;
        });
        return;
    }

    const auto id = *uploadId;
    if (!isValidUploadId(id)) {
        cb(s3Error(k404NotFound, "NoSuchUpload",
                   "The specified upload does not exist"));
        return;
    }
    offLoop(std::move(cb), [req, bucket, key, id]() -> HttpResponsePtr {
        auto t = resolve(req, bucket, key, id);
        if (t.error)
            return t.error;
        auto body = req->body();
        if (body.size() > (2u << 20))
            return s3Error(k400BadRequest, "MalformedXML", "Body too large");
        auto requested = parseCompleteBody(body);
        if (!requested)
            return s3Error(k400BadRequest, "MalformedXML",
                           "The XML you provided was not well-formed");

        auto guard = Globals::uploads->lock(id, true);
        if (!guard)
            return busy();
        auto plan = MultipartStore::plan(Globals::uploads->listParts(id),
                                         *requested, Globals::maxObjectBytes);
        if (!plan.error.empty())
            return s3Error(statusFor(plan.error), plan.error,
                           "The multipart upload cannot be completed");

        BlobStore::Staged staged;
        staged.tmp = Globals::blobs->newTempPath(bucket);
        auto a = Globals::uploads->assemble(id, plan.parts, staged.tmp);
        staged.etag = a.etag;
        staged.size = a.size;
        commitObject(t.bucketId, bucket, key, t.meta.contentType, staged);
        Globals::uploads->remove(id);

        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                          "<CompleteMultipartUploadResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\"><Location>/" + xmlEscape(bucket) + "/" + xmlEscape(key) +
               "</Location><Bucket>" + xmlEscape(bucket) + "</Bucket><Key>" +
               xmlEscape(key) + "</Key><ETag>&quot;" + a.etag +
               "&quot;</ETag></CompleteMultipartUploadResult>";
        auto r = HttpResponse::newHttpResponse();
        r->setContentTypeString("application/xml");
        r->addHeader("ETag", "\"" + a.etag + "\"");
        r->setBody(xml);
        return r;
    });
}

void MultipartCtrl::putPart(const HttpRequestPtr& req, S3Callback&& cb,
                            const std::string& bucket, const std::string& key)
{
    const auto id = queryValue(req->query(), "uploadId").value_or("");
    const auto n = parsePartNumber(
        queryValue(req->query(), "partNumber").value_or(""));
    if (!n || !isValidKey(key)) {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "partNumber must be 1..10000"));
        return;
    }
    if (!isValidUploadId(id)) {
        cb(s3Error(k404NotFound, "NoSuchUpload",
                   "The specified upload does not exist"));
        return;
    }
    if (req->body().size() > Globals::kMaxPartBytes) {
        cb(s3Error(k413RequestEntityTooLarge, "EntityTooLarge",
                   "Part exceeds the 100 MB limit"));
        return;
    }
    offLoop(std::move(cb), [req, bucket, key, id, n]() -> HttpResponsePtr {
        auto t = resolve(req, bucket, key, id);
        if (t.error)
            return t.error;
        auto guard = Globals::uploads->lock(id, false);
        if (!guard)
            return busy();
        auto res = Globals::uploads->putPart(id, *n, req->body(),
                                             Globals::maxObjectBytes);
        if (!res.error.empty())
            return s3Error(statusFor(res.error), res.error,
                           "The part could not be stored");
        auto r = HttpResponse::newHttpResponse();
        r->addHeader("ETag", "\"" + res.etag + "\"");
        return r;
    });
}

void MultipartCtrl::abort(const HttpRequestPtr& req, S3Callback&& cb,
                          const std::string& bucket, const std::string& key)
{
    const auto id = queryValue(req->query(), "uploadId").value_or("");
    if (!isValidUploadId(id)) {
        cb(s3Error(k404NotFound, "NoSuchUpload",
                   "The specified upload does not exist"));
        return;
    }
    offLoop(std::move(cb), [req, bucket, key, id]() -> HttpResponsePtr {
        auto t = resolve(req, bucket, key, id);
        if (t.error)
            return t.error;
        auto guard = Globals::uploads->lock(id, true);
        if (!guard)
            return busy();
        Globals::uploads->remove(id);
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(k204NoContent);
        return r;
    });
}

} // namespace s3
