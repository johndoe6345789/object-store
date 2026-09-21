/**
 * @file ObjectCtrl.h
 * @brief S3 object PUT, GET, DELETE, HEAD, list.
 */

#pragma once

#include <drogon/HttpController.h>

namespace s3
{

class ObjectCtrl : public drogon::HttpController<ObjectCtrl>
{
  public:
    METHOD_LIST_BEGIN
    // Regex routes, not "/{bucket}/{key:.*}": drogon turns every {name} into
    // ([^/]*), so that pattern never matched a key containing a slash and
    // "dir/file" answered 404. Here the key is everything after the bucket.
    ADD_METHOD_VIA_REGEX(ObjectCtrl::putObject, "/([^/]+)/(.+)", drogon::Put,
                         "s3::AuthFilter");
    ADD_METHOD_VIA_REGEX(ObjectCtrl::getObject, "/([^/]+)/(.+)", drogon::Get,
                         "s3::AuthFilter");
    ADD_METHOD_VIA_REGEX(ObjectCtrl::headObject, "/([^/]+)/(.+)", drogon::Head,
                         "s3::AuthFilter");
    ADD_METHOD_VIA_REGEX(ObjectCtrl::deleteObject, "/([^/]+)/(.+)", drogon::Delete,
                         "s3::AuthFilter");
    METHOD_LIST_END

    void putObject(const drogon::HttpRequestPtr&,
                   std::function<void(const drogon::HttpResponsePtr&)>&&,
                   const std::string& bucket, const std::string& key);

    void getObject(const drogon::HttpRequestPtr&,
                   std::function<void(const drogon::HttpResponsePtr&)>&&,
                   const std::string& bucket, const std::string& key);

    void headObject(const drogon::HttpRequestPtr&,
                    std::function<void(const drogon::HttpResponsePtr&)>&&,
                    const std::string& bucket, const std::string& key);

    void deleteObject(const drogon::HttpRequestPtr&,
                      std::function<void(const drogon::HttpResponsePtr&)>&&,
                      const std::string& bucket, const std::string& key);
};

} // namespace s3
