/**
 * @file MultipartStore.h
 * @brief On-disk multipart uploads: $DATA/.uploads/<id>/<partNumber>.
 *
 * Pure filesystem logic (no drogon, no DB) so it can be unit tested. Parts are
 * written to a temporary name and renamed, so re-sending a part is an atomic
 * overwrite. Nothing here holds a whole object in memory: assemble() streams
 * parts through a fixed buffer while hashing.
 */

#pragma once

#include "DigestUtil.h"
#include "NameUtil.h"

#include <json/json.h>

#include <sys/stat.h>

#include <algorithm>
#include <ctime>
#include <tuple>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace s3
{

namespace fs = std::filesystem;

class MultipartStore
{
  public:

    struct Meta {
        std::string owner, bucket, key, contentType;
        std::string metadata = "{}"; ///< JSON: system + x-amz-meta-* headers
        long long initiated = 0;     ///< epoch seconds
    };
    struct Part {
        int number;
        uintmax_t size;
        long long mtime = 0; ///< epoch seconds
    };
    /// Outcome of planning a completion: `error` is an S3 error code, empty
    /// on success.
    struct Plan {
        std::string error;
        std::vector<Part> parts;
        uintmax_t total = 0;
    };
    struct PutResult {
        std::string error; // S3 error code, empty on success
        std::string etag;
    };
    struct Assembled {
        std::string etag;
        uintmax_t size = 0;
    };

    /// @brief RAII per-id lock. Shared for part uploads (many in parallel),
    ///        exclusive for complete/abort/sweep; a conflict is refused, not
    ///        waited on, so a worker thread never parks behind a 1 GB copy.
    class Guard
    {
      public:
        Guard() = default;
        Guard(MultipartStore* s, std::string id, bool excl)
            : s_(s), id_(std::move(id)), excl_(excl) {}
        Guard(Guard&& o) noexcept
            : s_(o.s_), id_(std::move(o.id_)), excl_(o.excl_) { o.s_ = nullptr; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard()
        {
            if (s_)
                s_->unlock(id_, excl_);
        }

      private:
        MultipartStore* s_ = nullptr;
        std::string id_;
        bool excl_ = false;
    };

    explicit MultipartStore(const fs::path& dataDir)
        : root_(dataDir / ".uploads")
    {
        fs::create_directories(root_);
    }

    /// @brief Start an upload bound to (owner, bucket, key); returns its id
    ///        (256 random bits, hex).
    std::string create(const Meta& m)
    {
        auto id = randomHex(32);
        auto dir = root_ / id;
        fs::create_directories(dir);
        Json::Value j;
        j["owner"] = m.owner;
        j["bucket"] = m.bucket;
        j["key"] = m.key;
        j["contentType"] = m.contentType;
        j["metadata"] = m.metadata;
        j["initiated"] = static_cast<Json::Int64>(
            m.initiated ? m.initiated : ::time(nullptr));
        auto tmp = dir / "meta.tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f << Json::FastWriter().write(j);
        }
        fs::rename(tmp, dir / "meta");
        return id;
    }

    /// @brief Metadata of an existing upload; nullopt for a bad or unknown id.
    std::optional<Meta> load(const std::string& id) const
    {
        if (!isValidUploadId(id))
            return std::nullopt;
        std::ifstream f(root_ / id / "meta", std::ios::binary);
        if (!f)
            return std::nullopt;
        Json::Value j;
        if (!Json::Reader().parse(f, j) || !j.isObject())
            return std::nullopt;
        Meta m{j["owner"].asString(), j["bucket"].asString(),
               j["key"].asString(), j["contentType"].asString()};
        if (j["metadata"].isString())
            m.metadata = j["metadata"].asString();
        m.initiated = j["initiated"].asInt64();
        return m;
    }

    /// @brief Total bytes of the parts already stored, excluding part `skip`.
    uintmax_t storedBytes(const std::string& id, int skip = 0) const
    {
        uintmax_t t = 0;
        for (auto& p : listParts(id))
            if (p.number != skip)
                t += p.size;
        return t;
    }

    /// @brief Write part `n` (temp file, then rename over any previous one).
    PutResult putPart(const std::string& id, int n, std::string_view data,
                      uintmax_t maxTotal) const
    {
        if (!isValidUploadId(id) || n < 1 || n > kMaxPartNumber)
            return {"InvalidArgument", ""};
        if (storedBytes(id, n) + data.size() > maxTotal)
            return {"EntityTooLarge", ""};
        auto dir = root_ / id;
        auto tmp = dir / ("tmp." + std::to_string(n) + "." + randomHex(6));
        Md5 md5;
        {
            std::ofstream f(tmp, std::ios::binary);
            for (size_t off = 0; off < data.size();) {
                auto len = std::min<size_t>(1 << 20, data.size() - off);
                md5.update(data.substr(off, len));
                f.write(data.data() + off, (std::streamsize)len);
                off += len;
            }
            f.close();
            if (!f) {
                std::error_code ec;
                fs::remove(tmp, ec);
                return {"InternalError", ""};
            }
        }
        fs::rename(tmp, dir / std::to_string(n));
        auto etag = md5.hex();
        // Sidecar so Complete/ListParts need not re-read the part; ignored by
        // listParts (its name is not a bare part number).
        std::ofstream(dir / (std::to_string(n) + ".etag")) << etag;
        return {"", etag};
    }

    /// @brief md5 hex of a stored part (sidecar, else computed).
    std::string partEtag(const std::string& id, int n) const
    {
        std::ifstream f(root_ / id / (std::to_string(n) + ".etag"));
        std::string e;
        if (f && std::getline(f, e) && e.size() == 32)
            return e;
        std::ifstream in(root_ / id / std::to_string(n), std::ios::binary);
        Md5 md5;
        std::vector<char> buf(1 << 20);
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            if (auto got = static_cast<size_t>(in.gcount()))
                md5.update({buf.data(), got});
        }
        return md5.hex();
    }

    /// @brief Uploads of (owner, bucket), ordered by (key, id).
    std::vector<std::pair<std::string, Meta>>
    listUploads(const std::string& owner, const std::string& bucket) const
    {
        std::vector<std::pair<std::string, Meta>> out;
        std::error_code ec;
        for (fs::directory_iterator it(root_, ec), end; !ec && it != end;
             it.increment(ec)) {
            auto id = it->path().filename().string();
            if (!isValidUploadId(id))
                continue;
            auto m = load(id);
            if (m && m->owner == owner && m->bucket == bucket)
                out.emplace_back(id, *m);
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
            return std::tie(a.second.key, a.first) <
                   std::tie(b.second.key, b.first);
        });
        return out;
    }

    /// @brief Stored parts in ascending order (temp and meta files ignored).
    std::vector<Part> listParts(const std::string& id) const
    {
        std::vector<Part> out;
        if (!isValidUploadId(id))
            return out;
        std::error_code ec;
        for (fs::directory_iterator it(root_ / id, ec), end; !ec && it != end;
             it.increment(ec)) {
            auto name = it->path().filename().string();
            auto n = parsePartNumber(name);
            if (n && std::to_string(*n) == name && it->is_regular_file()) {
                struct stat st {};
                ::stat(it->path().c_str(), &st);
                out.push_back({*n, it->file_size(),
                               static_cast<long long>(st.st_mtime)});
            }
        }
        std::sort(out.begin(), out.end(),
                  [](auto& a, auto& b) { return a.number < b.number; });
        return out;
    }

    /// @brief Decide which parts make the object. `requested` empty means all
    ///        stored parts; otherwise it must be strictly ascending and every
    ///        part must exist.
    static Plan plan(const std::vector<Part>& stored,
                     const std::vector<int>& requested, uintmax_t maxBytes)
    {
        Plan p;
        if (requested.empty()) {
            p.parts = stored;
        } else {
            int last = 0;
            for (int n : requested) {
                if (n <= last) {
                    p.error = "InvalidPartOrder";
                    return p;
                }
                last = n;
                auto it = std::find_if(stored.begin(), stored.end(),
                                       [n](auto& s) { return s.number == n; });
                if (it == stored.end()) {
                    p.error = "InvalidPart";
                    return p;
                }
                p.parts.push_back(*it);
            }
        }
        if (p.parts.empty()) {
            p.error = "MalformedXML";
            return p;
        }
        for (auto& part : p.parts)
            p.total += part.size;
        if (p.total > maxBytes)
            p.error = "EntityTooLarge";
        return p;
    }

    /// @brief Stream-concatenate the planned parts into `out`, hashing as it
    ///        goes. Memory use is one 1 MiB buffer regardless of object size.
    Assembled assemble(const std::string& id, const std::vector<Part>& parts,
                       const fs::path& out) const
    {
        Md5 md5;
        Assembled a;
        std::ofstream o(out, std::ios::binary);
        std::vector<char> buf(1 << 20);
        for (auto& p : parts) {
            std::ifstream in(root_ / id / std::to_string(p.number),
                             std::ios::binary);
            if (!in)
                throw std::runtime_error("part missing");
            while (in) {
                in.read(buf.data(), (std::streamsize)buf.size());
                auto got = static_cast<size_t>(in.gcount());
                if (!got)
                    break;
                md5.update({buf.data(), got});
                o.write(buf.data(), (std::streamsize)got);
                a.size += got;
            }
        }
        o.close();
        if (!o)
            throw std::runtime_error("assemble write failed");
        a.etag = md5.hex();
        return a;
    }

    /// @brief Delete an upload's directory (caller holds the exclusive lock).
    void remove(const std::string& id) const
    {
        if (!isValidUploadId(id))
            return;
        std::error_code ec;
        fs::remove_all(root_ / id, ec);
    }

    /// @brief Delete upload directories not touched for `maxAge`. Returns how
    ///        many were removed. Skips ids that are in use.
    size_t sweep(std::chrono::seconds maxAge)
    {
        size_t removed = 0;
        std::error_code ec;
        auto cutoff = fs::file_time_type::clock::now() - maxAge;
        std::vector<std::string> ids;
        for (fs::directory_iterator it(root_, ec), end; !ec && it != end;
             it.increment(ec)) {
            auto name = it->path().filename().string();
            std::error_code e2;
            if (isValidUploadId(name) && it->is_directory(e2) &&
                fs::last_write_time(it->path(), e2) < cutoff)
                ids.push_back(name);
        }
        for (auto& id : ids)
            if (auto g = lock(id, true)) {
                remove(id);
                ++removed;
            }
        return removed;
    }

    /// @brief Try to lock an id; empty when it is busy in a conflicting way.
    std::optional<Guard> lock(const std::string& id, bool exclusive)
    {
        std::lock_guard<std::mutex> l(m_);
        int& st = locks_[id]; // -1 exclusive, n>0 shared holders
        if (exclusive ? st != 0 : st < 0)
            return std::nullopt;
        st = exclusive ? -1 : st + 1;
        return Guard(this, id, exclusive);
    }

  private:
    void unlock(const std::string& id, bool exclusive)
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = locks_.find(id);
        if (it == locks_.end())
            return;
        it->second = exclusive ? 0 : it->second - 1;
        if (it->second == 0)
            locks_.erase(it);
    }

    fs::path root_;
    std::mutex m_;
    std::map<std::string, int> locks_;
};

} // namespace s3
