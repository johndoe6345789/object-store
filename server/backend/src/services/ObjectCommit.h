/**
 * @file ObjectCommit.h
 * @brief The one place an object becomes visible or is removed.
 *
 * Plain PUT and multipart-complete both go through commitObject(), so they
 * share the content-addressing and pathInUse() rules. Blobs are named
 * bucket/<md5> and bucket names are globally unique, so a blob is never shared
 * across owners; within a bucket, identical bytes under two keys are one file.
 * Publishing, the row write and the "is this blob still referenced?" check all
 * run under one mutex, otherwise a delete of key A could unlink a blob between
 * key B's publish and B's row insert and leave B pointing at nothing.
 */

#pragma once

#include "BlobStore.h"
#include "Globals.h"
#include "ObjectStore.h"

#include <mutex>

namespace s3
{

inline std::mutex& blobCommitMutex()
{
    static std::mutex m;
    return m;
}

/// @brief Publish a staged blob and upsert the object row. An overwritten
///        blob is removed once nothing references it. Returns the etag.
inline std::string commitObject(int bucketId, const std::string& bucket,
                                const std::string& key,
                                const std::string& contentType,
                                BlobStore::Staged& staged)
{
    std::lock_guard<std::mutex> lock(blobCommitMutex());
    const auto etag = staged.etag;
    const auto size = static_cast<int64_t>(staged.size);
    auto previous = ObjectStore::get(bucketId, key);
    auto rel = Globals::blobs->publish(bucket, staged);
    ObjectStore::put(bucketId, key, etag, size, contentType, rel);
    if (!previous.isNull()) {
        auto old = previous["storage_path"].asString();
        if (old != rel && !ObjectStore::pathInUse(old))
            Globals::blobs->remove(old);
    }
    return etag;
}

/// @brief Delete an object row and, if unreferenced, its blob.
inline void commitDelete(int bucketId, const std::string& key)
{
    std::lock_guard<std::mutex> lock(blobCommitMutex());
    auto path = ObjectStore::remove(bucketId, key);
    if (!path.empty() && !ObjectStore::pathInUse(path))
        Globals::blobs->remove(path);
}

} // namespace s3
