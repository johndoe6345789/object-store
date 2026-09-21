/**
 * @file UriEncoding.h
 * @brief The percent-encoding rules SigV4 and S3 use.
 */

#pragma once

#include <string>
#include <string_view>

namespace s3::sigv4
{

/// @brief AWS URI-encode: A-Z a-z 0-9 - _ . ~ stay, everything else is %XX
///        (uppercase hex). '/' is kept unless `encodeSlash`.
inline std::string uriEncode(std::string_view s, bool encodeSlash)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || (c == '/' && !encodeSlash)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

/// @brief Percent-decode. A malformed escape is kept literally. `plusIsSpace`
///        is for query strings only: in a path '+' is a plus.
inline std::string uriDecode(std::string_view s, bool plusIsSpace)
{
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int a = val(s[i + 1]), b = val(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out += static_cast<char>(a * 16 + b);
                i += 2;
                continue;
            }
        }
        out += (plusIsSpace && s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

} // namespace s3::sigv4
