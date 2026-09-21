/**
 * @file XmlParse.h
 * @brief Just enough XML reading for S3 request bodies (CompleteMultipartUpload,
 *        Delete): flat, known element names, no nesting beyond one level.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace s3
{

/// @brief Undo the five predefined entities and numeric character references.
inline std::string xmlUnescape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        auto semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        auto ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            out += '&';
        else if (ent == "lt")
            out += '<';
        else if (ent == "gt")
            out += '>';
        else if (ent == "quot")
            out += '"';
        else if (ent == "apos")
            out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            unsigned long cp = 0;
            bool hex = ent[1] == 'x' || ent[1] == 'X';
            for (size_t k = hex ? 2 : 1; k < ent.size(); ++k) {
                char c = ent[k];
                int v = c >= '0' && c <= '9'   ? c - '0'
                        : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                      : -1;
                if (v < 0) {
                    cp = 0;
                    break;
                }
                cp = cp * (hex ? 16 : 10) + static_cast<unsigned long>(v);
            }
            if (cp == 0 || cp > 0x10FFFF) {
                out += s[i];
                continue;
            }
            if (cp < 0x80)
                out += static_cast<char>(cp);
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += s[i];
            continue;
        }
        i = semi;
    }
    return out;
}

/// @brief Inner text of every <name>...</name> in `xml`, in order. Returns
///        false if an opening tag has no closing tag.
inline bool xmlElements(std::string_view xml, std::string_view name,
                        std::vector<std::string_view>& out)
{
    const std::string open = "<" + std::string(name) + ">";
    const std::string close = "</" + std::string(name) + ">";
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string_view::npos) {
        pos += open.size();
        auto end = xml.find(close, pos);
        if (end == std::string_view::npos)
            return false;
        out.push_back(xml.substr(pos, end - pos));
        pos = end + close.size();
    }
    return true;
}

/// @brief Unescaped text of the first <name> inside `xml`, if present.
inline bool xmlText(std::string_view xml, std::string_view name,
                    std::string& out)
{
    std::vector<std::string_view> v;
    if (!xmlElements(xml, name, v) || v.empty())
        return false;
    out = xmlUnescape(v[0]);
    return true;
}

} // namespace s3
