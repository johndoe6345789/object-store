/**
 * @file ObjectGet.cpp
 * @brief GetObject / HeadObject: Range, If-* conditionals, stored metadata.
 */

#include "S3Ctx.h"

#include "../services/BucketStore.h"
#include "../services/Globals.h"
#include "../services/HttpUtil.h"
#include "../services/ObjectMeta.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"
#include "../sigv4/SigV4Verify.h"

using namespace drogon;

namespace s3
{

namespace {

HttpResponsePtr notModified(const Json::Value& meta)
{
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(k304NotModified);
    r->addHeader("ETag", "\"" + meta["etag"].asString() + "\"");
    r->addHeader("Last-Modified",
                 httpDate(static_cast<time_t>(meta["last_modified_epoch"].asInt64())));
    return r;
}

/// RFC 7232 precedence. Returns a response to send instead of the object, or
/// null to carry on.
HttpResponsePtr checkConditionals(const S3Ctx& c, const Json::Value& meta)
{
    const auto etag = meta["etag"].asString();
    const time_t mtime = static_cast<time_t>(meta["last_modified_epoch"].asInt64());
    auto ifMatch = c.header("If-Match");
    auto ifNone = c.header("If-None-Match");
    auto ifUnmod = c.header("If-Unmodified-Since");
    auto ifMod = c.header("If-Modified-Since");

    auto precondFailed = [] {
        return s3Error(k412PreconditionFailed, "PreconditionFailed",
                       "At least one of the pre-conditions you specified did "
                       "not hold");
    };
    if (!ifMatch.empty()) {
        if (!etagMatches(ifMatch, etag))
            return precondFailed();
    } else if (!ifUnmod.empty()) {
        if (auto t = sigv4::parseHttpDate(ifUnmod); t && mtime > *t)
            return precondFailed();
    }
    if (!ifNone.empty()) {
        if (etagMatches(ifNone, etag))
            return notModified(meta);
    } else if (!ifMod.empty()) {
        if (auto t = sigv4::parseHttpDate(ifMod); t && mtime <= *t)
            return notModified(meta);
    }
    return nullptr;
}

} // namespace

void getObject(CtxPtr c, S3Callback&& cb)
{
    // The bucket lookup, the metadata query and the blob read all block; none
    // of them may run on the IO loop (see services/Workers.h).
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        int bid = BucketStore::getId(c->bucket, c->owner);
        if (bid == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist", c->bucket);
        auto meta = ObjectStore::get(bid, c->key);
        if (meta.isNull())
            return s3Error(k404NotFound, "NoSuchKey",
                           "The specified key does not exist.", c->key);
        if (auto r = checkConditionals(*c, meta))
            return r;

        auto full = Globals::blobs->fullPath(meta["storage_path"].asString());
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec))
            // Metadata without bytes is a broken store, not an empty object.
            return s3Error(k500InternalServerError, "InternalError",
                           "The object data could not be read");

        const uint64_t size = static_cast<uint64_t>(meta["size"].asInt64());
        auto rng = parseRange(c->header("Range"), size);
        const auto ct = meta["content_type"].asString();
        HttpResponsePtr r;
        if (rng.kind == ByteRange::Invalid)
            return s3Error(k416RequestedRangeNotSatisfiable, "InvalidRange",
                           "The requested range is not satisfiable");
        if (rng.kind == ByteRange::Ok) {
            r = HttpResponse::newFileResponse(
                full.string(), static_cast<size_t>(rng.start),
                static_cast<size_t>(rng.end - rng.start + 1), false, "",
                CT_NONE, ct);
            if (r->statusCode() == k416RequestedRangeNotSatisfiable)
                return s3Error(k416RequestedRangeNotSatisfiable, "InvalidRange",
                               "The requested range is not satisfiable");
            r->setStatusCode(k206PartialContent);
            r->addHeader("Content-Range",
                         "bytes " + std::to_string(rng.start) + "-" +
                             std::to_string(rng.end) + "/" +
                             std::to_string(size));
        } else {
            // Streamed from disk (sendfile for big blobs). No manual
            // Content-Length: the file response sets one.
            r = HttpResponse::newFileResponse(full.string(), "", CT_NONE, ct);
        }
        r->addHeader("Accept-Ranges", "bytes");
        r->addHeader("ETag", "\"" + meta["etag"].asString() + "\"");
        r->addHeader("Last-Modified",
                     httpDate(static_cast<time_t>(
                         meta["last_modified_epoch"].asInt64())));
        applyMetadata(r, meta["metadata"]);
        // Query overrides (usually from a presigned URL, e.g. to force a
        // download name): they replace the stored values for this response.
        static const std::pair<const char*, const char*> kOverrides[] = {
            {"response-content-type", "Content-Type"},
            {"response-content-disposition", "Content-Disposition"},
            {"response-cache-control", "Cache-Control"},
            {"response-content-encoding", "Content-Encoding"},
            {"response-content-language", "Content-Language"},
            {"response-expires", "Expires"}};
        for (const auto& [param, header] : kOverrides)
            if (auto v = c->query.find(param); v && !v->empty() &&
                v->find_first_of("\r\n") == std::string::npos) {
                if (std::string(header) == "Content-Type")
                    r->setContentTypeString(*v);
                else
                    r->addHeader(header, *v);
            }
        // The stored whole-object checksum, when asked for (and not for a
        // partial body, whose checksum it would not match).
        if (rng.kind != ByteRange::Ok &&
            c->header("x-amz-checksum-mode") == "ENABLED") {
            const auto& cs = meta["metadata"]["checksum"];
            if (cs.isObject())
                for (const auto& n : cs.getMemberNames())
                    r->addHeader(n, cs[n].asString());
        }
        return r;
    });
}

} // namespace s3
