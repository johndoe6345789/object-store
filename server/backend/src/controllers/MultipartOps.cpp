/**
 * @file MultipartOps.cpp
 * @brief Multipart upload: initiate, upload part (+ copy), complete, abort.
 *        All work runs off the IO loops. ETag of a completed upload is S3's
 *        md5-of-part-md5s with a "-N" suffix.
 */

#include "S3Ctx.h"

#include "../services/BucketStore.h"
#include "../services/Globals.h"
#include "../services/HttpUtil.h"
#include "../services/MappedFile.h"
#include "../services/NameUtil.h"
#include "../services/ObjectCommit.h"
#include "../services/ObjectMeta.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"
#include "../services/XmlParse.h"
#include "../sigv4/Crypto.h"

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

/// Ownership for every multipart route: the bucket must belong to the
/// authenticated owner, and the upload must have been initiated by that same
/// (owner, bucket, key). A foreign or unknown upload is NoSuchUpload either
/// way, so ids of other owners' uploads are not confirmable.
struct Target {
    HttpResponsePtr error;
    int bucketId = 0;
    MultipartStore::Meta meta;
};

Target resolve(const S3Ctx& c, const std::string& uploadId)
{
    Target t;
    t.bucketId = BucketStore::getId(c.bucket, c.owner);
    if (t.bucketId == 0) {
        t.error = s3Error(k404NotFound, "NoSuchBucket",
                          "The specified bucket does not exist", c.bucket);
        return t;
    }
    auto m = Globals::uploads->load(uploadId);
    if (!m || m->owner != c.owner || m->bucket != c.bucket || m->key != c.key) {
        t.error = s3Error(k404NotFound, "NoSuchUpload",
                          "The specified upload does not exist. The upload ID "
                          "may be invalid, or the upload may have been aborted "
                          "or completed.",
                          uploadId);
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

HttpResponsePtr noSuchUpload()
{
    return s3Error(k404NotFound, "NoSuchUpload",
                   "The specified upload does not exist. The upload ID may be "
                   "invalid, or the upload may have been aborted or completed.");
}

} // namespace

void initiateMultipart(CtxPtr c, S3Callback&& cb)
{
    if (!isValidKey(c->key)) {
        cb(s3Error(k400BadRequest, "InvalidArgument", "Invalid object key"));
        return;
    }
    auto ct = c->header("Content-Type");
    if (ct.empty())
        ct = "application/octet-stream";
    auto meta = metadataFromRequest(c->req, c->header("Content-Encoding"));
    if (!meta) {
        cb(s3Error(k400BadRequest, "MetadataTooLarge",
                   "Your metadata headers exceed the maximum allowed metadata "
                   "size."));
        return;
    }
    const auto metaJson = toJsonString(*meta);
    offLoop(std::move(cb), [c, ct, metaJson]() -> HttpResponsePtr {
        if (BucketStore::getId(c->bucket, c->owner) == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist", c->bucket);
        auto id = Globals::uploads->create(
            {c->owner, c->bucket, c->key, ct, metaJson, 0});
        return xmlResponse(
            std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<InitiateMultipartUploadResult xmlns=\"") +
            kS3Namespace + "\"><Bucket>" + xmlEscape(c->bucket) +
            "</Bucket><Key>" + xmlEscape(c->key) + "</Key><UploadId>" + id +
            "</UploadId></InitiateMultipartUploadResult>");
    });
}

namespace {

struct PartArgs {
    std::string id;
    int number = 0;
};

bool partArgs(const S3Ctx& c, PartArgs& a, HttpResponsePtr& err)
{
    a.id = c.query.get("uploadId");
    auto n = parsePartNumber(c.query.get("partNumber"));
    if (!n || !isValidKey(c.key)) {
        err = s3Error(k400BadRequest, "InvalidArgument",
                      "Part number must be an integer between 1 and 10000, "
                      "inclusive");
        return false;
    }
    a.number = *n;
    if (!isValidUploadId(a.id)) {
        err = noSuchUpload();
        return false;
    }
    return true;
}

} // namespace

void uploadPart(CtxPtr c, S3Callback&& cb)
{
    PartArgs a;
    HttpResponsePtr err;
    if (!partArgs(*c, a, err)) {
        cb(err);
        return;
    }
    if (c->body().size() > Globals::kMaxPartBytes) {
        cb(s3Error(k413RequestEntityTooLarge, "EntityTooLarge",
                   "Part exceeds the 100 MB limit"));
        return;
    }
    offLoop(std::move(cb), [c, a]() -> HttpResponsePtr {
        auto t = resolve(*c, a.id);
        if (t.error)
            return t.error;
        auto guard = Globals::uploads->lock(a.id, false);
        if (!guard)
            return busy();
        auto res = Globals::uploads->putPart(a.id, a.number, c->body(),
                                             Globals::maxObjectBytes);
        if (!res.error.empty())
            return s3Error(statusFor(res.error), res.error,
                           "The part could not be stored");
        auto r = HttpResponse::newHttpResponse();
        r->addHeader("ETag", "\"" + res.etag + "\"");
        if (c->payload && c->payload->checksum)
            r->addHeader(c->payload->checksum->first,
                         c->payload->checksum->second);
        return r;
    });
}

void uploadPartCopy(CtxPtr c, S3Callback&& cb)
{
    PartArgs a;
    HttpResponsePtr err;
    if (!partArgs(*c, a, err)) {
        cb(err);
        return;
    }
    if (!c->mayRead()) {
        cb(s3Error(k403Forbidden, "AccessDenied", "Access Denied"));
        return;
    }
    auto raw = c->header("x-amz-copy-source");
    if (auto q = raw.find('?'); q != std::string::npos)
        raw.resize(q);
    if (!raw.empty() && raw[0] == '/')
        raw.erase(0, 1);
    auto slash = raw.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= raw.size()) {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Copy Source must mention the source bucket and key: "
                   "sourcebucket/sourcekey"));
        return;
    }
    const auto sb = sigv4::uriDecode(raw.substr(0, slash), false);
    const auto sk = sigv4::uriDecode(raw.substr(slash + 1), false);
    offLoop(std::move(cb), [c, a, sb, sk]() -> HttpResponsePtr {
        auto t = resolve(*c, a.id);
        if (t.error)
            return t.error;
        int srcBid = BucketStore::getId(sb, c->owner);
        if (srcBid == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist", sb);
        auto src = ObjectStore::get(srcBid, sk);
        if (src.isNull())
            return s3Error(k404NotFound, "NoSuchKey",
                           "The specified key does not exist.", sk);
        const uint64_t size = static_cast<uint64_t>(src["size"].asInt64());
        uint64_t start = 0, end = size ? size - 1 : 0;
        if (auto rh = c->header("x-amz-copy-source-range"); !rh.empty()) {
            auto r = parseRange(rh, size);
            if (r.kind != ByteRange::Ok)
                return s3Error(k400BadRequest, "InvalidArgument",
                               "The x-amz-copy-source-range value must be of "
                               "the form bytes=first-last where first and last "
                               "are the zero-based offsets of the first and "
                               "last bytes to copy");
            start = r.start;
            end = r.end;
        }
        const uint64_t len = size ? end - start + 1 : 0;
        if (len > Globals::kMaxPartBytes)
            return s3Error(k413RequestEntityTooLarge, "EntityTooLarge",
                           "Part exceeds the 100 MB limit");
        auto guard = Globals::uploads->lock(a.id, false);
        if (!guard)
            return busy();
        std::unique_ptr<MappedFile> map;
        try {
            map = std::make_unique<MappedFile>(
                Globals::blobs->fullPath(src["storage_path"].asString()), false);
        } catch (...) {
            return s3Error(k404NotFound, "NoSuchKey",
                           "The specified key does not exist.", sk);
        }
        auto res = Globals::uploads->putPart(
            a.id, a.number, map->view().substr(start, len),
            Globals::maxObjectBytes);
        if (!res.error.empty())
            return s3Error(statusFor(res.error), res.error,
                           "The part could not be stored");
        return xmlResponse(
            std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<CopyPartResult xmlns=\"") +
            kS3Namespace + "\"><LastModified>" + isoUtc(::time(nullptr)) +
            "</LastModified><ETag>&quot;" + res.etag +
            "&quot;</ETag></CopyPartResult>");
    });
}

void completeMultipart(CtxPtr c, S3Callback&& cb)
{
    const auto id = c->query.get("uploadId");
    if (!isValidUploadId(id)) {
        cb(noSuchUpload());
        return;
    }
    if (c->body().size() > (2u << 20)) {
        cb(s3Error(k400BadRequest, "MalformedXML", "Body too large"));
        return;
    }
    // <Part><PartNumber>n</PartNumber><ETag>"md5"</ETag></Part>... An empty
    // body means "all stored parts, ascending".
    struct Req {
        std::vector<int> numbers;
        std::vector<std::string> etags; // "" = not stated
    } req;
    {
        std::vector<std::string_view> parts;
        auto body = c->body();
        bool blank = true;
        for (char ch : body)
            if (!std::isspace(static_cast<unsigned char>(ch)))
                blank = false;
        if (!blank) {
            std::string dummy;
            if (!xmlElements(body, "Part", parts) || parts.empty()) {
                cb(s3Error(k400BadRequest, "MalformedXML",
                           "The XML you provided was not well-formed or did "
                           "not validate against our published schema"));
                return;
            }
            for (auto p : parts) {
                std::string num, etag;
                auto n = xmlText(p, "PartNumber", num)
                             ? parsePartNumber(num)
                             : std::nullopt;
                if (!n) {
                    cb(s3Error(k400BadRequest, "MalformedXML",
                               "The XML you provided was not well-formed or "
                               "did not validate against our published "
                               "schema"));
                    return;
                }
                xmlText(p, "ETag", etag);
                // Quotes may arrive literally or as &quot; (already decoded).
                while (!etag.empty() && etag.front() == '"')
                    etag.erase(0, 1);
                while (!etag.empty() && etag.back() == '"')
                    etag.pop_back();
                req.numbers.push_back(*n);
                req.etags.push_back(etag);
            }
        }
    }
    offLoop(std::move(cb), [c, id, req]() -> HttpResponsePtr {
        auto t = resolve(*c, id);
        if (t.error)
            return t.error;
        auto guard = Globals::uploads->lock(id, true);
        if (!guard)
            return busy();
        auto plan = MultipartStore::plan(Globals::uploads->listParts(id),
                                         req.numbers, Globals::maxObjectBytes);
        if (!plan.error.empty())
            return s3Error(statusFor(plan.error), plan.error,
                           "The multipart upload cannot be completed");

        // ETags the client names must be the ones the parts got, and the
        // object's ETag is S3's md5(md5(part1)+md5(part2)+...)-N.
        sigv4::Digest agg(EVP_md5());
        for (size_t i = 0; i < plan.parts.size(); ++i) {
            const auto pe = Globals::uploads->partEtag(id, plan.parts[i].number);
            if (i < req.etags.size() && !req.etags[i].empty() &&
                req.etags[i] != pe)
                return s3Error(k400BadRequest, "InvalidPart",
                               "One or more of the specified parts could not "
                               "be found. The part may not have been uploaded, "
                               "or the specified entity tag may not match the "
                               "part's entity tag.");
            std::string raw;
            sigv4::hexDecode(pe, raw);
            agg.update(raw);
        }
        const auto etag =
            sigv4::hexEncode(agg.finish()) + "-" + std::to_string(plan.parts.size());

        BlobStore::Staged staged;
        staged.tmp = Globals::blobs->newTempPath(c->bucket);
        auto a = Globals::uploads->assemble(id, plan.parts, staged.tmp);
        staged.etag = a.etag; // blob name: md5 of the assembled bytes
        staged.size = a.size;
        commitObject(t.bucketId, c->bucket, c->key, t.meta.contentType, staged,
                     t.meta.metadata, etag);
        Globals::uploads->remove(id);

        auto r = xmlResponse(
            std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<CompleteMultipartUploadResult xmlns=\"") +
            kS3Namespace + "\"><Location>/" + xmlEscape(c->bucket) + "/" +
            xmlEscape(c->key) + "</Location><Bucket>" + xmlEscape(c->bucket) +
            "</Bucket><Key>" + xmlEscape(c->key) + "</Key><ETag>&quot;" + etag +
            "&quot;</ETag></CompleteMultipartUploadResult>");
        r->addHeader("ETag", "\"" + etag + "\"");
        return r;
    });
}

void abortMultipart(CtxPtr c, S3Callback&& cb)
{
    const auto id = c->query.get("uploadId");
    if (!isValidUploadId(id)) {
        cb(noSuchUpload());
        return;
    }
    offLoop(std::move(cb), [c, id]() -> HttpResponsePtr {
        auto t = resolve(*c, id);
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
