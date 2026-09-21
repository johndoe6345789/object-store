/**
 * @file AuthUtil.h
 * @brief Pure helpers for the AuthFilter: secret comparison and permissions.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace s3
{

/// @brief Compare two secrets without an early exit on the first differing
///        byte. The length difference is folded into the result and the loop
///        always runs over the longer input, so timing does not reveal how
///        much of a guess was right.
inline bool constantTimeEquals(std::string_view a, std::string_view b)
{
    unsigned int diff = static_cast<unsigned int>(a.size() ^ b.size());
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char x = i < a.size() ? a[i] : 0;
        const unsigned char y = i < b.size() ? b[i] : 0;
        diff |= static_cast<unsigned int>(x ^ y);
    }
    return diff == 0;
}

/// @brief Does the comma/whitespace separated `permissions` list contain
///        exactly the token `token`? ("readonly" does not grant "read".)
inline bool hasPermissionToken(std::string_view permissions,
                               std::string_view token)
{
    size_t i = 0;
    while (i < permissions.size()) {
        while (i < permissions.size() &&
               (permissions[i] == ',' || permissions[i] == ' ' ||
                permissions[i] == '\t' || permissions[i] == '\n' ||
                permissions[i] == '\r'))
            ++i;
        size_t j = i;
        while (j < permissions.size() && permissions[j] != ',' &&
               permissions[j] != ' ' && permissions[j] != '\t' &&
               permissions[j] != '\n' && permissions[j] != '\r')
            ++j;
        if (j > i && permissions.substr(i, j - i) == token)
            return true;
        i = j;
    }
    return false;
}

/// @brief Reads (GET/HEAD) need `read`, everything else `write`; `admin`
///        satisfies both.
inline bool isAllowed(std::string_view permissions, bool isRead)
{
    return hasPermissionToken(permissions, "admin") ||
           hasPermissionToken(permissions, isRead ? "read" : "write");
}

} // namespace s3
