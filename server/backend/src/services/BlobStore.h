/**
 * @file BlobStore.h
 * @brief Filesystem blob storage with MD5 ETag.
 */

#pragma once

#include "DigestUtil.h"
#include "NameUtil.h"
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace s3
{

/// @brief Content-addressed filesystem blob store.
///
/// Writes are two-phase: bytes go to a temporary file inside the bucket
/// directory while the md5 is computed (a Staged), and publish() renames it to
/// its content-addressed name. A reader therefore never sees a half-written
/// blob, and nothing large is ever held in memory.
class BlobStore
{
  public:
    explicit BlobStore(const std::filesystem::path& root) : root_(root)
    {
        std::filesystem::create_directories(root);
    }

    /// @brief Store data, return {etag, size, path}.
    struct StoreResult {
        std::string etag;
        size_t size;
        std::string path;
    };

    /// @brief A fully written temporary blob. Removes its file on
    ///        destruction unless publish() consumed it.
    struct Staged {
        std::string etag;
        uintmax_t size = 0;
        std::filesystem::path tmp;

        Staged() = default;
        Staged(std::string e, uintmax_t s, std::filesystem::path t)
            : etag(std::move(e)), size(s), tmp(std::move(t)) {}
        Staged(Staged&& o) noexcept
            : etag(std::move(o.etag)), size(o.size), tmp(std::move(o.tmp))
        {
            o.tmp.clear();
        }
        Staged& operator=(Staged&& o) noexcept
        {
            discard();
            etag = std::move(o.etag);
            size = o.size;
            tmp = std::move(o.tmp);
            o.tmp.clear();
            return *this;
        }
        ~Staged() { discard(); }
        void discard()
        {
            if (!tmp.empty()) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                tmp.clear();
            }
        }
    };

    /// @brief A fresh temporary path inside the bucket directory (same
    ///        filesystem as the final name, so publish() can rename).
    std::filesystem::path newTempPath(const std::string& bucket)
    {
        auto dir = root_ / bucket;
        std::filesystem::create_directories(dir);
        return dir / (".tmp." + randomHex(8));
    }

    /// @brief Write `data` to a temp file, hashing in 1 MiB chunks.
    Staged stage(const std::string& bucket, std::string_view data)
    {
        Staged s;
        s.tmp = newTempPath(bucket);
        Md5 md5;
        std::ofstream f(s.tmp, std::ios::binary);
        for (size_t off = 0; off < data.size();) {
            auto n = std::min<size_t>(1 << 20, data.size() - off);
            md5.update(data.substr(off, n));
            f.write(data.data() + off, (std::streamsize)n);
            off += n;
        }
        f.close();
        if (!f)
            throw std::runtime_error("blob write failed");
        s.etag = md5.hex();
        s.size = data.size();
        return s;
    }

    /// @brief Atomically move a staged blob to bucket/<etag>; returns the
    ///        storage path. Replacing an existing identical blob is fine.
    std::string publish(const std::string& bucket, Staged& s)
    {
        auto rel = bucket + "/" + s.etag;
        std::filesystem::rename(s.tmp, root_ / rel);
        s.tmp.clear();
        return rel;
    }

    // Accept string_view so callers can write directly from the request body
    // buffer without making an intermediate copy of potentially large blobs.
    StoreResult store(const std::string& bucket, const std::string&,
                      std::string_view data)
    {
        auto s = stage(bucket, data);
        auto path = publish(bucket, s);
        return {s.etag, static_cast<size_t>(s.size), path};
    }

    /// @brief Absolute path for a storage path (for streaming responses).
    std::filesystem::path fullPath(const std::string& path) const
    {
        return root_ / path;
    }

    /// @brief Read blob by storage path; nullopt when it is not there.
    // Returning an empty string for a missing blob made a GET answer 200
    // with no body, so a lost blob looked like an empty object.
    std::optional<std::string> read(const std::string& path)
    {
        auto full = root_ / path;
        if (!std::filesystem::exists(full))
            return std::nullopt;
        std::ifstream f(full, std::ios::binary);
        if (!f)
            return std::nullopt;
        return std::string{std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>()};
    }

    /// @brief Delete blob by storage path.
    bool remove(const std::string& path)
    {
        return std::filesystem::remove(root_ / path);
    }

    /// @brief Delete every blob of a bucket (its directory).
    void removeBucket(const std::string& bucket)
    {
        if (!isValidBucketName(bucket))
            return; // never remove_all() a path built from odd input
        std::error_code ec;
        std::filesystem::remove_all(root_ / bucket, ec);
    }

  private:
    std::filesystem::path root_;
};

} // namespace s3
