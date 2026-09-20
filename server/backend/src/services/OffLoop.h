/**
 * @file OffLoop.h
 * @brief Run a handler's blocking work off drogon's IO loops.
 *
 * Usage:
 *     offLoop(std::move(cb), [req, bucket] { ...; return response; });
 *
 * The lambda runs on a Workers thread and its response is delivered from
 * there, which drogon permits from any thread. Nothing in a handler may run
 * on the IO loop: see Workers.h for what a blocked loop does to this server.
 */

#pragma once

#include "Workers.h"
#include <drogon/HttpResponse.h>
#include <functional>
#include <memory>

namespace s3
{

inline void offLoop(std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                    std::function<drogon::HttpResponsePtr()> work)
{
    auto cbPtr = std::make_shared<
        std::function<void(const drogon::HttpResponsePtr&)>>(std::move(cb));
    Workers::post([cbPtr, work = std::move(work)] {
        drogon::HttpResponsePtr r;
        try {
            r = work();
        } catch (...) {
            // Never drop the response: an unanswered request is a client
            // hanging until its own timeout, which is what a thrown DB
            // exception used to cause here.
            r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k500InternalServerError);
        }
        (*cbPtr)(r);
    });
}

} // namespace s3
