/**
 * @file ListCtrl.cpp
 * @brief GET /{bucket} -- list a bucket's objects.
 */

#include "ListCtrl.h"

#include <algorithm>

#include "../services/BucketStore.h"
#include "../services/ObjectStore.h"
#include "../services/S3Response.h"
#include "../services/XmlUtil.h"

using namespace drogon;

namespace s3
{

void ListCtrl::listObjects(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& cb,
                           const std::string& bucket)
{
    int bid = BucketStore::getId(
        bucket, req->attributes()->get<std::string>("owner"));
    if (bid == 0) {
        cb(s3Error(k404NotFound, "NoSuchBucket",
                   "The specified bucket does not exist", bucket));
        return;
    }

    auto prefix = req->getParameter("prefix");
    int maxKeys = 1000;
    auto mk = req->getParameter("max-keys");
    if (!mk.empty()) {
        try {
            maxKeys = std::clamp(std::stoi(mk), 1, 1000);
        } catch (...) {
            maxKeys = 1000;
        }
    }

    // ListObjectsV2 differs from V1 in what it is asked and what it answers:
    // it takes continuation-token/start-after rather than marker, and returns
    // KeyCount and NextContinuationToken.
    const bool v2 = req->getParameter("list-type") == "2";
    std::string startAfter = v2 ? req->getParameter("continuation-token")
                                : req->getParameter("marker");
    if (v2 && startAfter.empty())
        startAfter = req->getParameter("start-after");

    // One more than asked for, so truncation is known rather than assumed.
    // This used to report IsTruncated=false unconditionally, which told every
    // client a partial listing was the whole bucket.
    auto rows = ObjectStore::list(bid, prefix, maxKeys + 1, startAfter);
    const bool truncated = static_cast<int>(rows.size()) > maxKeys;
    if (truncated)
        rows.resize(static_cast<size_t>(maxKeys));

    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<ListBucketResult xmlns=\"";
    xml += kS3Namespace;
    xml += "\"><Name>" + xmlEscape(bucket) + "</Name>";
    xml += "<Prefix>" + xmlEscape(prefix) + "</Prefix>";
    xml += "<MaxKeys>" + std::to_string(maxKeys) + "</MaxKeys>";
    xml += std::string("<IsTruncated>") + (truncated ? "true" : "false") +
           "</IsTruncated>";
    if (v2)
        xml += "<KeyCount>" + std::to_string(rows.size()) + "</KeyCount>";
    else if (!startAfter.empty())
        xml += "<Marker>" + xmlEscape(startAfter) + "</Marker>";

    for (const auto& obj : rows) {
        xml += "<Contents>";
        xml += "<Key>" + xmlEscape(obj["key"].asString()) + "</Key>";
        xml += "<LastModified>" +
               xmlEscape(isoTimestamp(obj["last_modified"].asString())) +
               "</LastModified>";
        xml += "<ETag>&quot;" + xmlEscape(obj["etag"].asString()) +
               "&quot;</ETag>";
        xml += "<Size>" + std::to_string(obj["size"].asInt64()) + "</Size>";
        // Required by the schema. Everything here is one class; saying so is
        // better than omitting a field an SDK expects.
        xml += "<StorageClass>STANDARD</StorageClass>";
        xml += "</Contents>";
    }

    // The token is the last key returned: list() resumes strictly after it.
    if (truncated && !rows.empty()) {
        const std::string last = rows.back()["key"].asString();
        xml += v2 ? "<NextContinuationToken>" + xmlEscape(last) +
                        "</NextContinuationToken>"
                  : "<NextMarker>" + xmlEscape(last) + "</NextMarker>";
    }
    xml += "</ListBucketResult>";

    auto r = HttpResponse::newHttpResponse();
    r->setContentTypeString("application/xml");
    r->setBody(xml);
    cb(r);
}

} // namespace s3
