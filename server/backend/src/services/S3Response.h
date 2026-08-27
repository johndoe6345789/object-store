/**
 * @file S3Response.h
 * @brief S3 wire-format helpers: the XML namespace, error documents, and
 *        timestamps.
 *
 * These exist because the responses were S3-shaped rather than S3: errors
 * were plain text where the protocol specifies an XML document, listings
 * carried no namespace, and timestamps went out in PostgreSQL's format, which
 * no S3 SDK can parse into a date.
 */

#pragma once

#include <drogon/HttpResponse.h>
#include <string>

#include "XmlUtil.h"

namespace s3
{

/// The namespace every S3 response document carries. Parsers that bind
/// elements by namespace read nothing without it.
inline constexpr const char* kS3Namespace =
    "http://s3.amazonaws.com/doc/2006-03-01/";

/**
 * @brief An S3 error document.
 *
 * The protocol specifies Code, Message and Resource in XML. Returning the
 * code as a bare string means an SDK reports a parse failure instead of the
 * condition, and "NoSuchBucket" reaches the caller as an unhelpful blob of
 * text rather than a typed error.
 */
inline drogon::HttpResponsePtr s3Error(drogon::HttpStatusCode status,
                                       const std::string& code,
                                       const std::string& message,
                                       const std::string& resource = "")
{
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<Error xmlns=\"";
    xml += kS3Namespace;
    xml += "\"><Code>" + xmlEscape(code) + "</Code>";
    xml += "<Message>" + xmlEscape(message) + "</Message>";
    if (!resource.empty())
        xml += "<Resource>" + xmlEscape(resource) + "</Resource>";
    xml += "</Error>";

    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(status);
    r->setContentTypeString("application/xml");
    r->setBody(xml);
    return r;
}

/**
 * @brief PostgreSQL's timestamp rendered as the ISO8601 S3 specifies.
 *
 * "2026-08-27 19:46:58.845507+00" becomes "2026-08-27T19:46:58.845Z".
 * Anything unrecognised is passed through rather than mangled -- a wrong
 * date is worse than an unconverted one, and the caller can still see it.
 */
inline std::string isoTimestamp(const std::string& pg)
{
    if (pg.size() < 19 || pg[10] != ' ')
        return pg;
    std::string out = pg.substr(0, 19);
    out[10] = 'T';
    // Milliseconds, if the source carried sub-second precision. Only the
    // digits: a blind substr(20, 3) swallows the timezone when there are
    // fewer than three of them ("58.5+00" became ".5+0").
    if (pg.size() > 20 && pg[19] == '.') {
        std::string frac;
        for (size_t i = 20; i < pg.size() && frac.size() < 3; ++i) {
            if (pg[i] < '0' || pg[i] > '9')
                break;
            frac += pg[i];
        }
        if (!frac.empty()) {
            while (frac.size() < 3)
                frac += '0';
            out += '.' + frac;
        }
    }
    out += 'Z';
    return out;
}

} // namespace s3
