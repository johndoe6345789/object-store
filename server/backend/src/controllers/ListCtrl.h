/**
 * @file ListCtrl.h
 * @brief S3 ListObjectsV2 endpoint.
 */

#pragma once

#include <drogon/HttpController.h>

namespace s3
{

/// @brief Handles GET /{bucket}?prefix=&max-keys=
class ListCtrl : public drogon::HttpController<ListCtrl>
{
  public:
    METHOD_LIST_BEGIN
    // GET /{bucket} -- what S3 itself uses to list a bucket's contents.
    //
    // This was /list/{bucket}, which could never work: ObjectCtrl registers
    // /{bucket}/{key:.*}, so /list/tenant-system matched that instead with
    // bucket="list", and every listing answered NoSuchBucket. Getting an
    // object worked throughout, which is why the route looked fine.
    //
    // /{bucket} cannot collide the same way: {key:.*} may match an empty
    // string, but the pattern still requires the separating slash.
    ADD_METHOD_TO(ListCtrl::listObjects, "/{bucket}", drogon::Get,
                  "s3::AuthFilter");
    METHOD_LIST_END

    void listObjects(const drogon::HttpRequestPtr&,
                     std::function<void(const drogon::HttpResponsePtr&)>&&,
                     const std::string& bucket);
};

} // namespace s3
