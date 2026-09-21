/**
 * @file ObjectPut.cpp
 * @brief PutObject, CopyObject, DeleteObject, DeleteObjects.
 */

#include "S3Ctx.h"

#include "../services/BucketStore.h"
#include "../services/Globals.h"
#include "../services/NameUtil.h"
#include "../services/ObjectCommit.h"
#include "../services/ObjectMeta.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"
#include "../services/XmlParse.h"
#include "../services/XmlUtil.h"

using namespace drogon;

namespace s3
{

namespace {

HttpResponsePtr noBucket(const std::string& b)
{
    return s3Error(k404NotFound, "NoSuchBucket",
                   "The specified bucket does not exist", b);
}

/// Content-Type or the default.
std::string contentTypeOf(const S3Ctx& c)
{
    auto ct = c.header("Content-Type");
    return ct.empty() ? "application/octet-stream" : ct;
}

} // namespace

void putObject(CtxPtr c, S3Callback&& cb)
{
    if (!isValidKey(c->key)) {
        cb(s3Error(k400BadRequest, "InvalidArgument", "Invalid object key"));
        return;
    }
    if (c->body().size() > Globals::maxObjectBytes) {
        cb(s3Error(k413RequestEntityTooLarge, "EntityTooLarge",
                   "Your proposed upload exceeds the maximum allowed size"));
        return;
    }
    auto meta = metadataFromRequest(c->req, c->payload ? c->payload->contentEncoding
                                                       : std::string());
    if (!meta) {
        cb(s3Error(k400BadRequest, "MetadataTooLarge",
                   "Your metadata headers exceed the maximum allowed metadata "
                   "size."));
        return;
    }
    if (c->payload && c->payload->checksum)
        (*meta)["checksum"][c->payload->checksum->first] =
            c->payload->checksum->second;
    const auto metaJson = toJsonString(*meta);
    const auto ct = contentTypeOf(*c);

    // `c` keeps the request (and with it the body / decoded spool file) alive
    // until stage() has read it in 1 MiB chunks: a large PUT is never copied
    // into memory.
    offLoop(std::move(cb), [c, metaJson, ct]() -> HttpResponsePtr {
        int bid = BucketStore::getId(c->bucket, c->owner);
        if (bid == 0)
            return noBucket(c->bucket);
        auto staged = Globals::blobs->stage(c->bucket, c->body());
        auto etag = commitObject(bid, c->bucket, c->key, ct, staged, metaJson);
        auto r = HttpResponse::newHttpResponse();
        r->addHeader("ETag", "\"" + etag + "\"");
        if (c->payload && c->payload->checksum)
            r->addHeader(c->payload->checksum->first,
                         c->payload->checksum->second);
        return r;
    });
}

namespace {

/// x-amz-copy-source -> (bucket, key). Accepts "/b/k", "b/k", URL-encoded,
/// with an optional ?versionId (ignored).
bool parseCopySource(std::string raw, std::string& bucket, std::string& key)
{
    if (auto q = raw.find('?'); q != std::string::npos)
        raw.resize(q);
    if (!raw.empty() && raw[0] == '/')
        raw.erase(0, 1);
    auto slash = raw.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= raw.size())
        return false;
    bucket = sigv4::uriDecode(raw.substr(0, slash), false);
    key = sigv4::uriDecode(raw.substr(slash + 1), false);
    return isValidKey(key);
}

std::string isoNow(const Json::Value& meta) { return meta["last_modified"].asString(); }

} // namespace

void copyObject(CtxPtr c, S3Callback&& cb)
{
    std::string sb, sk;
    if (!isValidKey(c->key) || !parseCopySource(c->header("x-amz-copy-source"), sb, sk)) {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Copy Source must mention the source bucket and key: "
                   "sourcebucket/sourcekey"));
        return;
    }
    // Reading the source needs `read` as well as the `write` this PUT needed.
    if (!c->mayRead()) {
        cb(s3Error(k403Forbidden, "AccessDenied", "Access Denied"));
        return;
    }
    const auto directive = c->header("x-amz-metadata-directive");
    if (!directive.empty() && directive != "COPY" && directive != "REPLACE") {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Unknown metadata directive."));
        return;
    }
    const bool replace = directive == "REPLACE";
    std::optional<Json::Value> newMeta;
    if (replace) {
        newMeta = metadataFromRequest(c->req, c->header("Content-Encoding"));
        if (!newMeta) {
            cb(s3Error(k400BadRequest, "MetadataTooLarge",
                       "Your metadata headers exceed the maximum allowed "
                       "metadata size."));
            return;
        }
    }
    offLoop(std::move(cb), [c, sb, sk, replace, newMeta]() -> HttpResponsePtr {
        int srcBid = BucketStore::getId(sb, c->owner);
        if (srcBid == 0)
            return noBucket(sb);
        int dstBid = BucketStore::getId(c->bucket, c->owner);
        if (dstBid == 0)
            return noBucket(c->bucket);
        auto src = ObjectStore::get(srcBid, sk);
        if (src.isNull())
            return s3Error(k404NotFound, "NoSuchKey",
                           "The specified key does not exist.", sk);
        if (sb == c->bucket && sk == c->key && !replace)
            return s3Error(k400BadRequest, "InvalidRequest",
                           "This copy request is illegal because it is trying "
                           "to copy an object to itself without changing the "
                           "object's metadata, storage class, website "
                           "redirect location or encryption attributes.");

        Json::Value meta = replace ? *newMeta : src["metadata"];
        const auto ct = replace ? contentTypeOf(*c) : src["content_type"].asString();
        const auto storagePath = src["storage_path"].asString();
        // The blob is named by its content md5; the copy's ETag is that md5
        // (S3 does the same, even for a multipart source).
        const auto md5 = std::filesystem::path(storagePath).filename().string();
        const auto size = static_cast<int64_t>(src["size"].asInt64());
        if (replace) // a replaced metadata set keeps the stored checksum
            meta["checksum"] = src["metadata"]["checksum"];
        if (meta["checksum"].isNull())
            meta.removeMember("checksum");
        const auto metaJson = toJsonString(meta);

        if (sb == c->bucket) {
            if (!commitReference(dstBid, c->key, md5, size, ct, storagePath,
                                 metaJson))
                return s3Error(k404NotFound, "NoSuchKey",
                               "The specified key does not exist.", sk);
        } else {
            // Blobs live in per-bucket directories; a hard link puts the same
            // bytes in the destination's without copying them.
            auto tmp = Globals::blobs->newTempPath(c->bucket);
            std::error_code ec;
            std::filesystem::create_hard_link(Globals::blobs->fullPath(storagePath),
                                              tmp, ec);
            if (ec) {
                std::filesystem::copy_file(Globals::blobs->fullPath(storagePath),
                                           tmp, ec);
                if (ec)
                    return s3Error(k404NotFound, "NoSuchKey",
                                   "The specified key does not exist.", sk);
            }
            BlobStore::Staged staged(md5, static_cast<uintmax_t>(size), tmp);
            commitObject(dstBid, c->bucket, c->key, ct, staged, metaJson, md5);
        }
        auto dst = ObjectStore::get(dstBid, c->key);
        return xmlResponse(
            std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                        "<CopyObjectResult xmlns=\"") +
            kS3Namespace + "\"><LastModified>" + xmlEscape(isoNow(dst)) +
            "</LastModified><ETag>&quot;" + md5 + "&quot;</ETag>"
            "</CopyObjectResult>");
    });
}

void deleteObject(CtxPtr c, S3Callback&& cb)
{
    offLoop(std::move(cb), [c]() -> HttpResponsePtr {
        int bid = BucketStore::getId(c->bucket, c->owner);
        if (bid == 0)
            return noBucket(c->bucket);
        // Removes the blob only when no other key still points at it.
        commitDelete(bid, c->key);
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(k204NoContent);
        return r;
    });
}

void deleteObjects(CtxPtr c, S3Callback&& cb)
{
    auto body = c->body();
    if (body.size() > (4u << 20)) {
        cb(s3Error(k400BadRequest, "MalformedXML", "Body too large"));
        return;
    }
    std::vector<std::string_view> objs;
    std::string quiet;
    if (!xmlElements(body, "Object", objs) || objs.empty() ||
        objs.size() > 1000) {
        cb(s3Error(k400BadRequest, "MalformedXML",
                   "The XML you provided was not well-formed or did not "
                   "validate against our published schema"));
        return;
    }
    std::vector<std::string> keys;
    for (auto o : objs) {
        std::string k;
        if (!xmlText(o, "Key", k)) {
            cb(s3Error(k400BadRequest, "MalformedXML",
                       "The XML you provided was not well-formed or did not "
                       "validate against our published schema"));
            return;
        }
        keys.push_back(std::move(k));
    }
    xmlText(body, "Quiet", quiet);
    const bool isQuiet = quiet == "true";
    offLoop(std::move(cb), [c, keys, isQuiet]() -> HttpResponsePtr {
        int bid = BucketStore::getId(c->bucket, c->owner);
        if (bid == 0)
            return noBucket(c->bucket);
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                          "<DeleteResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\">";
        for (const auto& k : keys) {
            if (!isValidKey(k)) {
                xml += "<Error><Key>" + xmlEscape(k) +
                       "</Key><Code>InvalidArgument</Code><Message>Invalid "
                       "object key</Message></Error>";
                continue;
            }
            commitDelete(bid, k);
            if (!isQuiet)
                xml += "<Deleted><Key>" + xmlEscape(k) + "</Key></Deleted>";
        }
        xml += "</DeleteResult>";
        return xmlResponse(xml);
    });
}

} // namespace s3
