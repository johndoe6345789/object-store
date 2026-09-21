/**
 * @file ListOps.cpp
 * @brief ListObjects (v1) and ListObjectsV2, with prefix, delimiter,
 *        pagination and encoding-type=url.
 */

#include "S3Ctx.h"

#include "../services/BucketStore.h"
#include "../services/ObjectStore.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"
#include "../services/XmlUtil.h"

#include <algorithm>

using namespace drogon;

namespace s3
{

namespace {

struct Entry {
    bool isPrefix;
    std::string name;
    Json::Value row; // objects only
};

/// Smallest string greater than everything that starts with `cp` (byte
/// order); empty if there is none.
std::string jumpPast(std::string cp)
{
    while (!cp.empty() && static_cast<unsigned char>(cp.back()) == 0xFF)
        cp.pop_back();
    if (cp.empty())
        return cp;
    cp.back() = static_cast<char>(static_cast<unsigned char>(cp.back()) + 1);
    return cp;
}

bool endsWith(const std::string& s, const std::string& suf)
{
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

} // namespace

void listObjects(CtxPtr c, S3Callback&& cb)
{
    const auto& q = c->query;
    const bool v2 = q.get("list-type") == "2";
    if (q.has("list-type") && !v2) {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Invalid list-type; only 2 is supported"));
        return;
    }
    int maxKeys = 1000;
    if (auto mk = q.find("max-keys")) {
        try {
            size_t used = 0;
            int v = std::stoi(*mk, &used);
            if (used != mk->size() || v < 0)
                throw std::invalid_argument("max-keys");
            maxKeys = std::min(v, 1000);
        } catch (...) {
            cb(s3Error(k400BadRequest, "InvalidArgument",
                       "Provided max-keys not an integer or within integer "
                       "range"));
            return;
        }
    }
    const auto encType = q.get("encoding-type");
    if (!encType.empty() && encType != "url") {
        cb(s3Error(k400BadRequest, "InvalidArgument",
                   "Invalid Encoding Method specified in Request"));
        return;
    }

    offLoop(std::move(cb), [c, v2, maxKeys, encType]() -> HttpResponsePtr {
        const auto& q = c->query;
        int bid = BucketStore::getId(c->bucket, c->owner);
        if (bid == 0)
            return s3Error(k404NotFound, "NoSuchBucket",
                           "The specified bucket does not exist", c->bucket);

        const auto prefix = q.get("prefix");
        const auto delimiter = q.get("delimiter");
        const auto token = q.get("continuation-token");
        const auto startAfterParam = q.get("start-after");
        const auto marker = q.get("marker");
        std::string after = v2 ? (token.empty() ? startAfterParam : token) : marker;

        // Resuming after a CommonPrefix: skip everything under it.
        bool inclusive = false;
        if (!delimiter.empty() && after.size() > prefix.size() &&
            after.compare(0, prefix.size(), prefix) == 0 &&
            endsWith(after, delimiter)) {
            after = jumpPast(after);
            inclusive = true;
        }

        // One more than asked for, so truncation is known rather than assumed.
        std::vector<Entry> entries;
        const size_t want = static_cast<size_t>(maxKeys) + 1;
        bool exhausted = maxKeys == 0;
        while (!exhausted && entries.size() < want) {
            const int need = static_cast<int>(want - entries.size());
            auto rows = ObjectStore::list(bid, prefix, need, after, inclusive);
            bool jumped = false;
            for (auto& row : rows) {
                const auto key = row["key"].asString();
                size_t pos = delimiter.empty()
                                 ? std::string::npos
                                 : key.find(delimiter, prefix.size());
                if (pos != std::string::npos) {
                    auto cp = key.substr(0, pos + delimiter.size());
                    entries.push_back({true, cp, {}});
                    after = jumpPast(cp);
                    inclusive = true;
                    jumped = true;
                    if (after.empty())
                        exhausted = true;
                    break;
                }
                entries.push_back({false, key, row});
                after = key;
                inclusive = false;
                if (entries.size() >= want)
                    break;
            }
            if (!jumped && static_cast<int>(rows.size()) < need)
                exhausted = true;
            if (rows.empty())
                exhausted = true;
        }
        const bool truncated = entries.size() > static_cast<size_t>(maxKeys);
        if (truncated)
            entries.resize(static_cast<size_t>(maxKeys));

        const bool urlEnc = encType == "url";
        auto enc = [&](const std::string& s) {
            return xmlEscape(urlEnc ? sigv4::uriEncode(s, false) : s);
        };
        size_t nObjects = 0, nPrefixes = 0;
        std::string body;
        for (auto& e : entries) {
            if (e.isPrefix) {
                ++nPrefixes;
                continue;
            }
            ++nObjects;
            body += "<Contents><Key>" + enc(e.name) + "</Key><LastModified>" +
                    xmlEscape(e.row["last_modified"].asString()) +
                    "</LastModified><ETag>&quot;" +
                    xmlEscape(e.row["etag"].asString()) + "&quot;</ETag><Size>" +
                    std::to_string(e.row["size"].asInt64()) +
                    "</Size><StorageClass>STANDARD</StorageClass></Contents>";
        }
        for (auto& e : entries)
            if (e.isPrefix)
                body += "<CommonPrefixes><Prefix>" + enc(e.name) +
                        "</Prefix></CommonPrefixes>";

        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                          "<ListBucketResult xmlns=\"";
        xml += kS3Namespace;
        xml += "\"><Name>" + xmlEscape(c->bucket) + "</Name>";
        xml += "<Prefix>" + enc(prefix) + "</Prefix>";
        if (v2) {
            if (!token.empty())
                xml += "<ContinuationToken>" + xmlEscape(token) +
                       "</ContinuationToken>";
            if (!startAfterParam.empty())
                xml += "<StartAfter>" + enc(startAfterParam) + "</StartAfter>";
            xml += "<KeyCount>" + std::to_string(nObjects + nPrefixes) +
                   "</KeyCount>";
        } else {
            xml += "<Marker>" + enc(marker) + "</Marker>";
        }
        xml += "<MaxKeys>" + std::to_string(maxKeys) + "</MaxKeys>";
        if (!delimiter.empty())
            xml += "<Delimiter>" + enc(delimiter) + "</Delimiter>";
        if (urlEnc)
            xml += "<EncodingType>url</EncodingType>";
        xml += std::string("<IsTruncated>") + (truncated ? "true" : "false") +
               "</IsTruncated>";
        if (truncated && !entries.empty()) {
            // The token is the last key/prefix returned; the next call
            // resumes strictly after it.
            const auto& last = entries.back().name;
            xml += v2 ? "<NextContinuationToken>" + xmlEscape(last) +
                            "</NextContinuationToken>"
                      : "<NextMarker>" + enc(last) + "</NextMarker>";
        }
        xml += body + "</ListBucketResult>";
        return xmlResponse(xml);
    });
}

} // namespace s3
