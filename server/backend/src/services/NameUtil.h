/**
 * @file NameUtil.h
 * @brief Strict validation and small parsers for names that reach the
 *        filesystem or SQL: bucket names, keys, upload ids, part numbers.
 */

#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace s3
{

inline constexpr int kMaxPartNumber = 10000;
inline constexpr size_t kMaxKeyBytes = 1024;

/// @brief Upload ids are lowercase hex, 32..128 chars. Nothing else, so an id
///        can never carry a path separator or "..".
inline bool isValidUploadId(std::string_view id)
{
    if (id.size() < 32 || id.size() > 128)
        return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

/// @brief Bucket names become directory names under the blob root, so they
///        are restricted to [A-Za-z0-9._-], no leading dot, no "..".
inline bool isValidBucketName(std::string_view n)
{
    if (n.empty() || n.size() > 128 || n.front() == '.')
        return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '_' || c == '-'))
            return false;
    return n.find("..") == std::string_view::npos;
}

/// @brief Keys are only ever SQL parameters, but reject what postgres or a
///        log line cannot hold sanely: empty, oversized, control characters.
inline bool isValidKey(std::string_view k)
{
    if (k.empty() || k.size() > kMaxKeyBytes)
        return false;
    for (char c : k)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return false;
    return true;
}

/// @brief Parse a part number: digits only, 1..10000.
inline std::optional<int> parsePartNumber(std::string_view s)
{
    if (s.empty() || s.size() > 5)
        return std::nullopt;
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return std::nullopt;
        n = n * 10 + (c - '0');
    }
    if (n < 1 || n > kMaxPartNumber)
        return std::nullopt;
    return n;
}

/// @brief Value of `name` in a raw query string (no percent-decoding: the
///        callers only take hex ids and digits). A bare `name` yields "".
inline std::optional<std::string> queryValue(std::string_view query,
                                             std::string_view name)
{
    size_t i = 0;
    while (i <= query.size()) {
        size_t j = query.find('&', i);
        if (j == std::string_view::npos)
            j = query.size();
        auto pair = query.substr(i, j - i);
        auto eq = pair.find('=');
        if (pair.substr(0, eq) == name)
            return eq == std::string_view::npos
                       ? std::string()
                       : std::string(pair.substr(eq + 1));
        i = j + 1;
    }
    return std::nullopt;
}

/// @brief Part numbers listed in a CompleteMultipartUpload body. nullopt when
///        the body is malformed; an empty vector when it is blank.
inline std::optional<std::vector<int>> parseCompleteBody(std::string_view b)
{
    std::vector<int> out;
    static constexpr std::string_view open = "<PartNumber>";
    static constexpr std::string_view close = "</PartNumber>";
    size_t pos = 0;
    while ((pos = b.find(open, pos)) != std::string_view::npos) {
        pos += open.size();
        auto end = b.find(close, pos);
        if (end == std::string_view::npos)
            return std::nullopt;
        auto n = parsePartNumber(b.substr(pos, end - pos));
        if (!n)
            return std::nullopt;
        out.push_back(*n);
        pos = end + close.size();
    }
    if (out.empty())
        for (char c : b)
            if (!std::isspace(static_cast<unsigned char>(c)))
                return std::nullopt;
    return out;
}

/// @brief Escape LIKE wildcards so a listing prefix matches literally.
inline std::string likeEscape(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '%' || c == '_')
            out += '\\';
        out += c;
    }
    return out;
}

} // namespace s3
