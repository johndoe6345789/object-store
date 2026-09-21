#include "../src/services/DigestUtil.h"
#include "../src/services/NameUtil.h"
#include <cassert>
#include <iostream>

/// Everything that decides whether a client-supplied string may reach the
/// filesystem or SQL.
int nameUtilTest()
{
    using namespace s3;
    const std::string id(32, 'a');
    assert(isValidUploadId(id));
    assert(isValidUploadId(randomHex(32)));
    assert(!isValidUploadId(""));
    assert(!isValidUploadId(std::string(31, 'a')));
    assert(!isValidUploadId(std::string(129, 'a')));
    assert(!isValidUploadId(std::string(32, 'A')));           // hex is lowercase
    assert(!isValidUploadId(std::string(31, 'a') + "g"));
    assert(!isValidUploadId("../" + std::string(30, 'a')));
    assert(!isValidUploadId(std::string(31, 'a') + "/"));
    assert(!isValidUploadId(std::string(31, 'a') + std::string(1, '\0')));

    assert(isValidBucketName("tenant-system"));
    assert(isValidBucketName("a.b_c-1"));
    assert(!isValidBucketName(""));
    assert(!isValidBucketName(".."));
    assert(!isValidBucketName(".hidden"));
    assert(!isValidBucketName("a..b"));
    assert(!isValidBucketName("a/b"));
    assert(!isValidBucketName("a b"));
    assert(!isValidBucketName(std::string(129, 'a')));

    assert(isValidKey("dir/file name.txt"));
    assert(isValidKey("../x")); // only ever a SQL parameter, never a path
    assert(!isValidKey(""));
    assert(!isValidKey(std::string(1025, 'k')));
    assert(!isValidKey(std::string("a\0b", 3)));
    assert(!isValidKey("a\nb"));

    assert(parsePartNumber("1") == 1);
    assert(parsePartNumber("10000") == 10000);
    assert(!parsePartNumber("0"));
    assert(!parsePartNumber("10001"));
    assert(!parsePartNumber(""));
    assert(!parsePartNumber("-1"));
    assert(!parsePartNumber("1a"));
    assert(!parsePartNumber("123456"));

    assert(queryValue("uploads", "uploads") == "");
    assert(queryValue("a=1&uploadId=abc&b=2", "uploadId") == "abc");
    assert(!queryValue("a=1", "uploadId"));
    assert(!queryValue("", "uploads"));
    assert(!queryValue("xuploads=1", "uploads"));

    auto p = parseCompleteBody(
        "<CompleteMultipartUpload><Part><PartNumber>1</PartNumber>"
        "<ETag>\"x\"</ETag></Part><Part><PartNumber>3</PartNumber></Part>"
        "</CompleteMultipartUpload>");
    assert(p && *p == (std::vector<int>{1, 3}));
    assert(parseCompleteBody("") && parseCompleteBody("  \n")->empty());
    assert(!parseCompleteBody("garbage"));
    assert(!parseCompleteBody("<PartNumber>x</PartNumber>"));
    assert(!parseCompleteBody("<PartNumber>1"));

    assert(likeEscape("50%_off\\") == "50\\%\\_off\\\\");
    std::cout << "NameUtil: all cases pass\n";
    return 0;
}
