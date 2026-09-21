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
                                BlobStore::Staged& staged,
                                const std::string& metadataJson = "{}",
                                const std::string& objectEtag = "")
{
    std::lock_guard<std::mutex> lock(blobCommitMutex());
    // The blob is named by its content md5 (staged.etag); the object's own
    // ETag differs only for multipart objects (md5-of-md5s-N).
    const auto etag = objectEtag.empty() ? staged.etag : objectEtag;
    const auto size = static_cast<int64_t>(staged.size);
    auto previous = ObjectStore::get(bucketId, key);
    auto rel = Globals::blobs->publish(bucket, staged);
    ObjectStore::put(bucketId, key, etag, size, contentType, rel, metadataJson);
    if (!previous.isNull()) {
        auto old = previous["storage_path"].asString();
        if (old != rel && !ObjectStore::pathInUse(old))
            Globals::blobs->remove(old);
    }
    return etag;
}

/// @brief Point `key` at a blob that already exists in the same bucket (a
///        same-bucket CopyObject: no bytes move). Fails (false) if the source
///        blob is gone. Same locking as commitObject().
inline bool commitReference(int bucketId, const std::string& key,
                            const std::string& etag, int64_t size,
                            const std::string& contentType,
                            const std::string& storagePath,
                            const std::string& metadataJson)
{
    std::lock_guard<std::mutex> lock(blobCommitMutex());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(Globals::blobs->fullPath(storagePath), ec))
        return false;
    auto previous = ObjectStore::get(bucketId, key);
    ObjectStore::put(bucketId, key, etag, size, contentType, storagePath,
                     metadataJson);
    if (!previous.isNull()) {
        auto old = previous["storage_path"].asString();
        if (old != storagePath && !ObjectStore::pathInUse(old))
            Globals::blobs->remove(old);
    }
    return true;
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
