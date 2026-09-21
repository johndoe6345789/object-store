/**
 * @file MultipartCtrl.h
 * @brief S3-style multipart upload: initiate, upload part, complete, abort.
 */

#pragma once

#include <drogon/HttpController.h>

namespace s3
{

using S3Callback = std::function<void(const drogon::HttpResponsePtr&)>;

/// POST /{bucket}/{key}?uploads          -> initiate
/// POST /{bucket}/{key}?uploadId=ID      -> complete
/// PUT  /{bucket}/{key}?partNumber=N&uploadId=ID  (routed from ObjectCtrl)
/// DELETE /{bucket}/{key}?uploadId=ID    -> abort (routed from ObjectCtrl)
class MultipartCtrl : public drogon::HttpController<MultipartCtrl>
{
  public:
    METHOD_LIST_BEGIN
    // Regex route so the key may contain slashes (see ObjectCtrl.h).
    ADD_METHOD_VIA_REGEX(MultipartCtrl::post, "/([^/]+)/(.+)", drogon::Post,
                         "s3::AuthFilter");
    METHOD_LIST_END

    void post(const drogon::HttpRequestPtr&, S3Callback&&,
              const std::string& bucket, const std::string& key);

    static void putPart(const drogon::HttpRequestPtr&, S3Callback&&,
                        const std::string& bucket, const std::string& key);
    static void abort(const drogon::HttpRequestPtr&, S3Callback&&,
                      const std::string& bucket, const std::string& key);
};

} // namespace s3
