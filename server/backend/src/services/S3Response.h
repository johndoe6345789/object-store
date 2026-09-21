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

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <string>

#include "DigestUtil.h"
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
    // Like S3, no namespace on <Error>; RequestId/HostId are added on the way
    // out by stampResponse(), which knows the request.
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<Error>";
    xml += "<Code>" + xmlEscape(code) + "</Code>";
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

/// @brief s3Error for the plain int statuses the auth layer carries.
inline drogon::HttpResponsePtr s3ErrorStatus(int status, const std::string& code,
                                             const std::string& message,
                                             const std::string& resource = "")
{
    return s3Error(static_cast<drogon::HttpStatusCode>(status), code, message,
                   resource);
}

/// @brief A 16-character upper-case hex id, like S3's x-amz-request-id.
inline std::string newRequestId()
{
    auto h = randomHex(8);
    for (auto& c : h)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return h;
}

/**
 * @brief Runs on every response just before it is sent (registered as a
 *        drogon pre-sending advice): adds x-amz-request-id / x-amz-id-2 and,
 *        for error documents, the matching <RequestId>/<HostId> elements.
 */
inline void stampResponse(const drogon::HttpRequestPtr& req,
                          const drogon::HttpResponsePtr& resp)
{
    std::string id;
    auto attrs = req->attributes();
    if (attrs->find("request_id"))
        id = attrs->get<std::string>("request_id");
    else {
        id = newRequestId();
        attrs->insert("request_id", id);
    }
    resp->addHeader("x-amz-request-id", id);
    resp->addHeader("x-amz-id-2", id + randomHex(12));
    constexpr std::string_view tail = "</Error>";
    auto body = resp->body();
    if (body.size() > tail.size() &&
        body.substr(body.size() - tail.size()) == tail &&
        body.find("<RequestId>") == std::string_view::npos) {
        std::string b(body.substr(0, body.size() - tail.size()));
        b += "<RequestId>" + id + "</RequestId><HostId>" + id +
             "</HostId></Error>";
        resp->setBody(std::move(b));
    }
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
