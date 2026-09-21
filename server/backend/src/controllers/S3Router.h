/**
 * @file S3Router.h
 * @brief The one HTTP route: everything under / is an S3 request.
 */

#pragma once

#include <drogon/HttpController.h>

namespace s3
{

/// One catch-all route rather than a route per operation: S3 picks the
/// operation from the method, the path shape (service / bucket / object) and
/// a query "subresource" (?uploads, ?delete, ?uploadId, ...), which drogon's
/// path router cannot express. It also lets the path be taken raw from the
/// request line: drogon's own decoding turns '+' into a space, which would
/// corrupt keys. /health is answered here too and skips authentication.
class S3Router : public drogon::HttpController<S3Router>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_VIA_REGEX(S3Router::handle, "/(.*)", drogon::Get, drogon::Put,
                         drogon::Post, drogon::Delete, drogon::Head,
                         "s3::AuthFilter");
    METHOD_LIST_END

    void handle(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&&,
                const std::string& ignored);
};

} // namespace s3
