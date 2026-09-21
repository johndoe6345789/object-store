/**
 * @file AuthFilter.h
 * @brief AWS Signature V4 authentication for every S3 route.
 */

#pragma once

#include <drogon/HttpFilter.h>

namespace s3
{

/// @brief Authenticates with SigV4 (Authorization header or presigned query),
///        applies key permissions, and verifies/decodes the body payload.
///        Sets the request attributes "access_key", "owner", "permissions"
///        and "payload" (the decoded body, see sigv4/PayloadReader.h).
///        /health is the only unauthenticated path.
class AuthFilter : public drogon::HttpFilter<AuthFilter>
{
  public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&& cb,
                  drogon::FilterChainCallback&& ccb) override;
};

} // namespace s3
