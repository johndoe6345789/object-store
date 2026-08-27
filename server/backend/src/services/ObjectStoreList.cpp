/**
 * @file ObjectStoreList.cpp
 * @brief ObjectStore::list() implementation.
 */

#include "ObjectStore.h"
#include "ObjectStoreUtil.h"

namespace s3
{

std::vector<Json::Value>
ObjectStore::list(int bucketId, const std::string& prefix, int maxKeys,
                  const std::string& startAfter)
{
    // Parameter numbering shifts with the optional clauses, so it is built up
    // rather than written out, and the values are bound in the same order.
    std::string sql = "SELECT * FROM objects WHERE bucket_id=$1";
    int n = 1;
    if (!prefix.empty())
        sql += " AND key LIKE $" + std::to_string(++n);
    if (!startAfter.empty())
        sql += " AND key > $" + std::to_string(++n);
    sql += " ORDER BY key LIMIT " + std::to_string(maxKeys);

    drogon::orm::Result r = [&] {
        if (!prefix.empty() && !startAfter.empty())
            return DbPool::get()->execSqlSync(sql, bucketId, prefix + "%",
                                              startAfter);
        if (!prefix.empty())
            return DbPool::get()->execSqlSync(sql, bucketId, prefix + "%");
        if (!startAfter.empty())
            return DbPool::get()->execSqlSync(sql, bucketId, startAfter);
        return DbPool::get()->execSqlSync(sql, bucketId);
    }();

    std::vector<Json::Value> out;
    for (const auto& row : r)
        out.push_back(ObjectStoreUtil::rowToJson(row));
    return out;
}

} // namespace s3
