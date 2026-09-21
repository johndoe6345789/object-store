/**
 * @file AuthFilter.cpp
 * @brief SigV4 header / presigned authentication, permissions, payload check.
 *
 * The legacy `Authorization: AWS <access>:<secret>` scheme is gone: anything
 * that is not AWS4-HMAC-SHA256 is refused.
 */

#include "AuthFilter.h"
#include "../services/AuthUtil.h"
#include "../services/Globals.h"
#include "../services/KeyStore.h"
#include "../services/OffLoop.h"
#include "../services/S3Response.h"
#include "../sigv4/PayloadReader.h"
#include "../sigv4/SigV4Verify.h"

#include <ctime>
#include <iostream>

using namespace drogon;

namespace s3
{

namespace {

HttpResponsePtr respond(const sigv4::AuthError& e)
{
    return s3ErrorStatus(e.status, e.code, e.message);
}

} // namespace

void AuthFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& cb,
                          FilterChainCallback&& ccb)
{
    const std::string rawPath = req->getOriginalPath();
    if (rawPath == "/health") {
        ccb();
        return;
    }

    // Cheap, purely syntactic checks stay on the loop; everything that
    // touches the database or hashes a body goes to a worker.
    auto parsed = std::make_shared<sigv4::ParsedAuth>();
    const auto& auth = req->getHeader("Authorization");
    if (!auth.empty()) {
        if (auth.size() > 4096) {
            cb(s3ErrorStatus(400, "AuthorizationHeaderMalformed",
                             "The authorization header is malformed"));
            return;
        }
        if (auto e = sigv4::parseAuthorizationHeader(auth, *parsed)) {
            cb(respond(*e));
            return;
        }
    } else if (sigv4::isPresignedQuery(req->query())) {
        if (auto e = sigv4::parsePresignedQuery(req->query(), *parsed)) {
            cb(respond(*e));
            return;
        }
    } else {
        cb(s3Error(k403Forbidden, "AccessDenied", "Access Denied"));
        return;
    }

    auto cbPtr = std::make_shared<FilterCallback>(std::move(cb));
    auto ccbPtr = std::make_shared<FilterChainCallback>(std::move(ccb));
    Workers::post([req, parsed, rawPath, cbPtr, ccbPtr] {
        auto fail = [&](const HttpResponsePtr& r) { (*cbPtr)(r); };
        try {
            auto rec = KeyStore::lookup(parsed->accessKey);
            if (!rec) {
                fail(s3Error(k403Forbidden, "InvalidAccessKeyId",
                             "The AWS Access Key Id you provided does not "
                             "exist in our records."));
                return;
            }

            sigv4::HeaderMap headers;
            for (const auto& [k, v] : req->headers())
                headers[k] = v;
            sigv4::VerifyInput vi;
            // drogon rewrites HEAD to GET before routing; the client signed HEAD.
            vi.method = req->isHead() ? "HEAD" : req->methodString();
            vi.rawPath = rawPath;
            vi.rawQuery = req->query();
            vi.headers = &headers;
            vi.now = ::time(nullptr);
            auto out = sigv4::verify(*parsed, rec->secret, vi, Globals::sigConfig);
            if (out.error) {
                fail(respond(*out.error));
                return;
            }

            // Exact tokens: "readonly" must not satisfy "read".
            const bool isRead = req->method() == Get || req->method() == Head;
            if (!isAllowed(rec->permissions, isRead)) {
                fail(s3Error(k403Forbidden, "AccessDenied", "Access Denied"));
                return;
            }

            sigv4::PayloadOptions po;
            po.payloadHash = out.payloadHash;
            po.headers = &headers;
            po.tmpDir = Globals::tmpDir;
            po.maxDecodedBytes = Globals::maxObjectBytes;
            po.checkChecksumHeaders = req->method() != Post;
            po.chunk.signingKey = out.signingKey;
            po.chunk.amzDate = out.amzDate;
            po.chunk.scope = out.scope;
            po.chunk.seedSignature = out.signature;
            auto pr = sigv4::preparePayload(req->body(), po);
            if (pr.error) {
                fail(respond(*pr.error));
                return;
            }

            req->attributes()->insert("access_key", parsed->accessKey);
            req->attributes()->insert("owner", rec->owner);
            req->attributes()->insert("permissions", rec->permissions);
            req->attributes()->insert("payload", pr.payload);
            (*ccbPtr)();
        } catch (const KeyStoreError& e) {
            // Misconfiguration (encrypted secret, wrong/missing master key).
            std::cerr << "[s3server] auth: " << e.what() << "\n";
            fail(s3Error(k500InternalServerError, "InternalError",
                         "We encountered an internal error. Please try again."));
        } catch (...) {
            fail(s3Error(k500InternalServerError, "InternalError",
                         "We encountered an internal error. Please try again."));
        }
    });
}

} // namespace s3
