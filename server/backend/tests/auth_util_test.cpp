#include "../src/services/AuthUtil.h"
#include <cassert>
#include <iostream>

/// Secret comparison and permission-token matching used by the AuthFilter.
int authUtilTest()
{
    using namespace s3;
    assert(constantTimeEquals("secret", "secret"));
    assert(constantTimeEquals("", ""));
    assert(!constantTimeEquals("secret", "secreT"));
    assert(!constantTimeEquals("secret", "secre"));   // prefix
    assert(!constantTimeEquals("secre", "secret"));
    assert(!constantTimeEquals("", "x"));
    assert(!constantTimeEquals(std::string("a\0b", 3), std::string("a\0c", 3)));

    // Exact tokens: the old substring find() let "readonly" satisfy "read".
    assert(hasPermissionToken("read,write", "read"));
    assert(hasPermissionToken("read, write", "write"));
    assert(hasPermissionToken("  read\twrite\n", "write"));
    assert(!hasPermissionToken("readonly", "read"));
    assert(!hasPermissionToken("overwrite", "write"));
    assert(!hasPermissionToken("superadmin", "admin"));
    assert(!hasPermissionToken("", "read"));
    assert(!hasPermissionToken(",,", "read"));
    assert(!hasPermissionToken("Read", "read")); // case-sensitive

    assert(isAllowed("read", true));
    assert(!isAllowed("read", false));
    assert(isAllowed("write", false));
    assert(!isAllowed("write", true));
    assert(isAllowed("admin", true) && isAllowed("admin", false));
    assert(!isAllowed("readonly,writeonly", true));
    assert(!isAllowed("readonly,writeonly", false));
    std::cout << "AuthUtil: all cases pass\n";
    return 0;
}
