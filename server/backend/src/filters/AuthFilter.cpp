/**
 * @file AuthFilter.cpp
 * @brief Simple access-key auth via Authorization header.
 */

#include "AuthFilter.h"
#include "../services/AuthUtil.h"
#include "../services/DbPool.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"

using namespace drogon;

namespace s3
{

void AuthFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& cb,
                          FilterChainCallback&& ccb)
{
    const auto& auth = req->getHeader("Authorization");
    if (auth.empty() || auth.size() > 1024) {
        auto r = s3Error(k403Forbidden, "AccessDenied",
                          "Access denied");
        cb(r);
        return;
    }

    // The web client uses an intentionally small internal credential scheme:
    // "AWS access_key:secret_key". This is not AWS Signature V4, so reject
    // ambiguous bearer credentials instead of pretending they are equivalent.
    std::string key;
    std::string suppliedSecret;
    if (auth.starts_with("AWS ")) {
        auto colon = auth.find(':', 4);
        if (colon == std::string::npos) {
            auto r = s3Error(k403Forbidden, "InvalidAuthorizationHeader",
                          "The authorization header is malformed");
            cb(r);
            return;
        }
        key = auth.substr(4, colon - 4);
        suppliedSecret = auth.substr(colon + 1);
    } else {
        auto r = s3Error(k403Forbidden, "InvalidAuthorizationHeader",
                          "The authorization header is malformed");
        cb(r);
        return;
    }

    // The api_keys lookup blocks, and this filter runs on every request: on
    // an IO loop one slow query stops that loop serving any connection again
    // (services/Workers.h). Both callbacks are safe to call from a worker.
    auto cbPtr = std::make_shared<FilterCallback>(std::move(cb));
    auto ccbPtr = std::make_shared<FilterChainCallback>(std::move(ccb));
    Workers::post([req, key, suppliedSecret, cbPtr, ccbPtr] {
        auto cb = [&](const HttpResponsePtr& r) { (*cbPtr)(r); };
        auto ccb = [&] { (*ccbPtr)(); };
        try {
            auto rows =
                DbPool::get()->execSqlSync("SELECT access_key, secret_key, owner, permissions "
                                           "FROM api_keys WHERE access_key=$1",
                                           key);
            // One response, one code path for "no such key" and "wrong
            // secret": the caller learns nothing about which access keys
            // exist. A missing key still runs a comparison so the two cases
            // take the same time.
            const std::string stored =
                rows.empty() ? std::string(suppliedSecret.size(), '\0')
                             : rows[0]["secret_key"].as<std::string>();
            const bool secretOk = constantTimeEquals(stored, suppliedSecret);
            if (rows.empty() || !secretOk) {
                auto r = s3Error(k403Forbidden, "InvalidAccessKeyId",
                                 "The access key id or secret you provided "
                                 "is not valid");
                cb(r);
                return;
            }
            // Exact tokens: "readonly" must not satisfy "read".
            const auto permissions = rows[0]["permissions"].as<std::string>();
            const bool isRead = req->getMethod() == Get || req->getMethod() == Head;
            if (!isAllowed(permissions, isRead)) {
                auto r = s3Error(k403Forbidden, "AccessDenied",
                              "Access denied");
                cb(r);
                return;
            }
            req->attributes()->insert("access_key", key);
            req->attributes()->insert(
                "owner", rows[0]["owner"].as<std::string>());
            ccb();
        } catch (...) {
            auto r = HttpResponse::newHttpResponse();
            r->setStatusCode(k500InternalServerError);
            cb(r);
        }
    });
}

} // namespace s3
