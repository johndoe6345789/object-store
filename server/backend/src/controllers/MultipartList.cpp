/**
 * @file MultipartList.cpp
 * @brief ListParts and ListMultipartUploads.
 */

#include "S3Ctx.h"

#include "../services/BucketStore.h"
#include "../services/Globals.h"
#include "../services/HttpUtil.h"
#include "../services/NameUtil.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"

#include <algorithm>

using namespace drogon;

namespace s3
{

namespace {

/// Parse a non-negative integer query value with a default and a ceiling.
bool intParam(const Query& q, const char* name, int def, int cap, int& out)
{
    out = def;
    auto v = q.find(name);
    if (!v)
        return true;
    if (v->empty() || v->size() > 9)
        return false;
    int n = 0;
    for (char ch : *v) {
        if (ch < '0' || ch > '9')
            return false;
        n = n * 10 + (ch - '0');
    }
    out = std::min(n, cap);
    return true;
}

} // namespace

void listParts(CtxPtr c, S3Callback&& cb)
{
    const auto id = c->query.get("uploadId");
    int maxParts = 0, marker = 0;
    if (!intParam(c->query, "max-parts", 1000, 1000, maxParts) ||
        !intParam(c->query, "part-number-marker", 0, kMaxPartNumber, marker)) {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Provided max-parts or part-number-marker is not valid"));
        return;
    }
    if (!isValidUploadId(id)) {
        cb(s3Error(k404NotFound, "NoSuchUpload",
                   "The specified upload does not exist."));
        return;
    }
    offLoop(std::move(cb), [c, id, maxParts, marker]() -> HttpResponsePtr {
        if (BucketStore::getId(c->bucket, c->owner) == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist", c->bucket);
        auto m = Globals::uploads->load(id);
        if (!m || m->owner != c->owner || m->bucket != c->bucket ||
            m->key != c->key)
            return s3Error(k404NotFound, "NoSuchUpload",
                           "The specified upload does not exist.", id);
        std::vector<MultipartStore::Part> parts;
        for (auto& p : Globals::uploads->listParts(id))
            if (p.number > marker)
                parts.push_back(p);
        const bool truncated = static_cast<int>(parts.size()) > maxParts;
        if (truncated)
            parts.resize(static_cast<size_t>(maxParts));
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                          "<ListPartsResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\"><Bucket>" + xmlEscape(c->bucket) + "</Bucket><Key>" +
               xmlEscape(c->key) + "</Key><UploadId>" + id + "</UploadId>";
        xml += "<Initiator><ID>" + xmlEscape(c->owner) + "</ID><DisplayName>" +
               xmlEscape(c->owner) + "</DisplayName></Initiator><Owner><ID>" +
               xmlEscape(c->owner) + "</ID><DisplayName>" + xmlEscape(c->owner) +
               "</DisplayName></Owner><StorageClass>STANDARD</StorageClass>";
        xml += "<PartNumberMarker>" + std::to_string(marker) +
               "</PartNumberMarker>";
        if (truncated && !parts.empty())
            xml += "<NextPartNumberMarker>" +
                   std::to_string(parts.back().number) +
                   "</NextPartNumberMarker>";
        xml += "<MaxParts>" + std::to_string(maxParts) + "</MaxParts>";
        xml += std::string("<IsTruncated>") + (truncated ? "true" : "false") +
               "</IsTruncated>";
        for (auto& p : parts)
            xml += "<Part><PartNumber>" + std::to_string(p.number) +
                   "</PartNumber><LastModified>" +
                   isoUtc(static_cast<time_t>(p.mtime)) +
                   "</LastModified><ETag>&quot;" +
                   Globals::uploads->partEtag(id, p.number) +
                   "&quot;</ETag><Size>" + std::to_string(p.size) +
                   "</Size></Part>";
        xml += "</ListPartsResult>";
        return xmlResponse(xml);
    });
}

void listMultipartUploads(CtxPtr c, S3Callback&& cb)
{
    int maxUploads = 0;
    if (!intParam(c->query, "max-uploads", 1000, 1000, maxUploads)) {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Provided max-uploads is not valid"));
        return;
    }
    offLoop(std::move(cb), [c, maxUploads]() -> HttpResponsePtr {
        if (BucketStore::getId(c->bucket, c->owner) == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist", c->bucket);
        const auto prefix = c->query.get("prefix");
        const auto keyMarker = c->query.get("key-marker");
        const auto idMarker = c->query.get("upload-id-marker");
        const bool urlEnc = c->query.get("encoding-type") == "url";
        auto enc = [&](const std::string& s) {
            return xmlEscape(urlEnc ? sigv4::uriEncode(s, false) : s);
        };
        std::vector<std::pair<std::string, MultipartStore::Meta>> uploads;
        for (auto& u : Globals::uploads->listUploads(c->owner, c->bucket)) {
            const auto& k = u.second.key;
            if (k.compare(0, prefix.size(), prefix) != 0)
                continue;
            if (!keyMarker.empty()) {
                if (k < keyMarker)
                    continue;
                if (k == keyMarker && (idMarker.empty() || u.first <= idMarker))
                    continue;
            }
            uploads.push_back(u);
        }
        const bool truncated = static_cast<int>(uploads.size()) > maxUploads;
        if (truncated)
            uploads.resize(static_cast<size_t>(maxUploads));
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                          "<ListMultipartUploadsResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\"><Bucket>" + xmlEscape(c->bucket) + "</Bucket><KeyMarker>" +
               enc(keyMarker) + "</KeyMarker><UploadIdMarker>" +
               xmlEscape(idMarker) + "</UploadIdMarker>";
        if (truncated && !uploads.empty())
            xml += "<NextKeyMarker>" + enc(uploads.back().second.key) +
                   "</NextKeyMarker><NextUploadIdMarker>" + uploads.back().first +
                   "</NextUploadIdMarker>";
        xml += "<MaxUploads>" + std::to_string(maxUploads) + "</MaxUploads>";
        if (!prefix.empty())
            xml += "<Prefix>" + enc(prefix) + "</Prefix>";
        if (urlEnc)
            xml += "<EncodingType>url</EncodingType>";
        xml += std::string("<IsTruncated>") + (truncated ? "true" : "false") +
               "</IsTruncated>";
        for (auto& [id, m] : uploads)
            xml += "<Upload><Key>" + enc(m.key) + "</Key><UploadId>" + id +
                   "</UploadId><Initiator><ID>" + xmlEscape(m.owner) +
                   "</ID><DisplayName>" + xmlEscape(m.owner) +
                   "</DisplayName></Initiator><Owner><ID>" + xmlEscape(m.owner) +
                   "</ID><DisplayName>" + xmlEscape(m.owner) +
                   "</DisplayName></Owner><StorageClass>STANDARD</StorageClass>"
                   "<Initiated>" +
                   isoUtc(static_cast<time_t>(m.initiated)) + "</Initiated></Upload>";
        xml += "</ListMultipartUploadsResult>";
        return xmlResponse(xml);
    });
}

} // namespace s3
