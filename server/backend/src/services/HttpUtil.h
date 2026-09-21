/**
 * @file HttpUtil.h
 * @brief Pure HTTP helpers for object GET/HEAD: RFC 7231 dates, Range,
 *        ETag matching for the If-* conditionals.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace s3
{

/// @brief "Thu, 21 Sep 2026 12:00:00 GMT" (always English, always GMT).
inline std::string httpDate(time_t t)
{
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tmv {};
    gmtime_r(&t, &tmv);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  days[tmv.tm_wday], tmv.tm_mday, mons[tmv.tm_mon],
                  tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

/// @brief "2026-09-21T12:00:00.000Z", the XML LastModified/Initiated form.
inline std::string isoUtc(time_t t)
{
    struct tm tmv {};
    gmtime_r(&t, &tmv);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                  tmv.tm_min, tmv.tm_sec);
    return buf;
}

struct ByteRange {
    enum Kind { None, Invalid, Ok } kind = None;
    uint64_t start = 0, end = 0; ///< inclusive, valid when kind == Ok
};

/// @brief Interpret a Range header against an object of `size` bytes.
///        Only a single "bytes=" range is honoured; anything else is None
///        (the whole object is served, as S3 does). A range that starts past
///        the end is Invalid (416).
inline ByteRange parseRange(std::string_view h, uint64_t size)
{
    constexpr std::string_view p = "bytes=";
    ByteRange r;
    if (h.substr(0, p.size()) != p || h.find(',') != std::string_view::npos)
        return r;
    h.remove_prefix(p.size());
    auto dash = h.find('-');
    if (dash == std::string_view::npos)
        return r;
    auto num = [](std::string_view s) -> std::optional<uint64_t> {
        if (s.empty() || s.size() > 18)
            return std::nullopt;
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9')
                return std::nullopt;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        return v;
    };
    auto a = h.substr(0, dash), b = h.substr(dash + 1);
    if (a.empty()) { // suffix: last n bytes
        auto n = num(b);
        if (!n)
            return r;
        if (*n == 0 || size == 0) {
            r.kind = ByteRange::Invalid;
            return r;
        }
        r.kind = ByteRange::Ok;
        r.start = *n >= size ? 0 : size - *n;
        r.end = size - 1;
        return r;
    }
    auto s = num(a);
    if (!s)
        return r;
    uint64_t e = size ? size - 1 : 0;
    if (!b.empty()) {
        auto en = num(b);
        if (!en || *en < *s)
            return r; // syntactically invalid: ignored
        e = *en;
    }
    if (*s >= size) {
        r.kind = ByteRange::Invalid;
        return r;
    }
    r.kind = ByteRange::Ok;
    r.start = *s;
    r.end = e >= size ? size - 1 : e;
    return r;
}

/// @brief Does an If-Match / If-None-Match value (list, "*", W/ weak, quotes)
///        match `etag` (unquoted)?
inline bool etagMatches(std::string_view header, std::string_view etag)
{
    size_t i = 0;
    while (i <= header.size()) {
        size_t j = header.find(',', i);
        if (j == std::string_view::npos)
            j = header.size();
        auto tok = header.substr(i, j - i);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.remove_suffix(1);
        if (tok.substr(0, 2) == "W/")
            tok.remove_prefix(2);
        if (tok == "*")
            return true;
        if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"')
            tok = tok.substr(1, tok.size() - 2);
        if (!tok.empty() && tok == etag)
            return true;
        i = j + 1;
    }
    return false;
}

} // namespace s3
