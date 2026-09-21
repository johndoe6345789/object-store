/**
 * @file S3Router.cpp
 * @brief Maps (method, path shape, subresource) to an S3 operation.
 */

#include "S3Router.h"

#include "S3Ctx.h"

using namespace drogon;

namespace s3
{

namespace {

/// Subresources of real S3 operations this store does not implement. Refusing
/// them beats treating e.g. PUT /bucket?versioning as CreateBucket.
bool unsupportedSubresource(const Query& q)
{
    static const char* names[] = {
        "acl", "cors", "lifecycle", "policy", "tagging", "versioning",
        "versions", "website", "encryption", "notification", "logging",
        "replication", "accelerate", "requestPayment", "analytics", "metrics",
        "inventory", "ownershipControls", "publicAccessBlock", "object-lock",
        "intelligent-tiering", "policyStatus", "select", "restore", "torrent",
        "legal-hold", "retention", "attributes", "versionId"};
    for (auto n : names)
        if (q.has(n))
            return true;
    return false;
}

HttpResponsePtr notImplemented()
{
    return s3Error(k501NotImplemented, "NotImplemented",
                   "A header or query parameter you provided implies "
                   "functionality that is not implemented");
}

HttpResponsePtr methodNotAllowed()
{
    return s3Error(k405MethodNotAllowed, "MethodNotAllowed",
                   "The specified method is not allowed against this resource.");
}

} // namespace

void S3Router::handle(const HttpRequestPtr& req, S3Callback&& cb,
                      const std::string&)
{
    const std::string raw = req->getOriginalPath();
    if (raw == "/health") {
        cb(HttpResponse::newHttpJsonResponse(Json::Value("ok")));
        return;
    }

    auto c = std::make_shared<S3Ctx>();
    c->req = req;
    c->query = Query::parse(req->query());
    c->owner = req->attributes()->get<std::string>("owner");
    c->permissions = req->attributes()->get<std::string>("permissions");
    c->payload = req->attributes()->get<std::shared_ptr<sigv4::Payload>>("payload");

    // Path-style: /{bucket}[/{key}]. Decoded here, from the raw target: a
    // '+' in a path is a plus, and %2F inside a key stays a slash.
    std::string_view path = raw;
    path.remove_prefix(1);
    const auto slash = path.find('/');
    c->bucket = sigv4::uriDecode(path.substr(0, slash), false);
    if (slash != std::string_view::npos)
        c->key = sigv4::uriDecode(path.substr(slash + 1), false);

    const auto m = req->method();
    const bool head = c->head();
    const auto& q = c->query;

    if (unsupportedSubresource(q)) {
        cb(notImplemented());
        return;
    }

    // --- service level: GET / ---
    if (c->bucket.empty()) {
        if (m == Get)
            listBuckets(c, std::move(cb));
        else
            cb(methodNotAllowed());
        return;
    }

    // --- bucket level: /bucket and /bucket/ ---
    if (c->key.empty()) {
        if (head)
            headBucket(c, std::move(cb));
        else if (m == Get && q.has("location"))
            getBucketLocation(c, std::move(cb));
        else if (m == Get && q.has("uploads"))
            listMultipartUploads(c, std::move(cb));
        else if (m == Get)
            listObjects(c, std::move(cb));
        else if (m == Put)
            createBucket(c, std::move(cb));
        else if (m == Delete)
            deleteBucket(c, std::move(cb));
        else if (m == Post && q.has("delete"))
            deleteObjects(c, std::move(cb));
        else
            cb(m == Post ? notImplemented() : methodNotAllowed());
        return;
    }

    // --- object level ---
    const bool hasUpload = q.has("uploadId");
    const bool copy = !req->getHeader("x-amz-copy-source").empty();
    if (head || (m == Get && !hasUpload))
        getObject(c, std::move(cb));
    else if (m == Get)
        listParts(c, std::move(cb));
    else if (m == Put && (hasUpload || q.has("partNumber")))
        copy ? uploadPartCopy(c, std::move(cb)) : uploadPart(c, std::move(cb));
    else if (m == Put)
        copy ? copyObject(c, std::move(cb)) : putObject(c, std::move(cb));
    else if (m == Post && q.has("uploads"))
        initiateMultipart(c, std::move(cb));
    else if (m == Post && hasUpload)
        completeMultipart(c, std::move(cb));
    else if (m == Delete && hasUpload)
        abortMultipart(c, std::move(cb));
    else if (m == Delete)
        deleteObject(c, std::move(cb));
    else
        cb(m == Post ? notImplemented() : methodNotAllowed());
}

} // namespace s3
