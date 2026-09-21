#include "../src/services/HttpUtil.h"
#include "../src/services/SecretCrypto.h"
#include "../src/services/XmlParse.h"

#include <cassert>
#include <iostream>

int s3MiscTest()
{
    using namespace s3;

    // ---- Range -----------------------------------------------------------
    auto r = parseRange("bytes=0-9", 100);
    assert(r.kind == ByteRange::Ok && r.start == 0 && r.end == 9);
    r = parseRange("bytes=90-", 100);
    assert(r.kind == ByteRange::Ok && r.start == 90 && r.end == 99);
    r = parseRange("bytes=-10", 100);
    assert(r.kind == ByteRange::Ok && r.start == 90 && r.end == 99);
    r = parseRange("bytes=-500", 100);
    assert(r.kind == ByteRange::Ok && r.start == 0 && r.end == 99);
    r = parseRange("bytes=10-5000", 100);
    assert(r.kind == ByteRange::Ok && r.end == 99);
    assert(parseRange("bytes=100-", 100).kind == ByteRange::Invalid);
    assert(parseRange("bytes=0-0", 0).kind == ByteRange::Invalid);
    assert(parseRange("bytes=-0", 100).kind == ByteRange::Invalid);
    assert(parseRange("bytes=0-1,5-6", 100).kind == ByteRange::None);
    assert(parseRange("bytes=9-3", 100).kind == ByteRange::None);
    assert(parseRange("lines=0-3", 100).kind == ByteRange::None);
    assert(parseRange("", 100).kind == ByteRange::None);
    assert(parseRange("bytes=x-3", 100).kind == ByteRange::None);

    // ---- ETag matching / dates -------------------------------------------
    assert(etagMatches("\"abc\"", "abc"));
    assert(etagMatches("W/\"abc\"", "abc"));
    assert(etagMatches("\"x\", \"abc\"", "abc"));
    assert(etagMatches("*", "abc"));
    assert(!etagMatches("\"abd\"", "abc"));
    assert(!etagMatches("", "abc"));
    assert(httpDate(1369353600) == "Fri, 24 May 2013 00:00:00 GMT");
    assert(isoUtc(1369353600) == "2013-05-24T00:00:00.000Z");

    // ---- XML request bodies ------------------------------------------------
    assert(xmlUnescape("a&amp;b&lt;c&gt;&quot;&apos;") == "a&b<c>\"'");
    assert(xmlUnescape("&#65;&#x42;&#xE9;") == "AB\xC3\xA9");
    assert(xmlUnescape("&bogus; &") == "&bogus; &");
    std::vector<std::string_view> v;
    assert(xmlElements("<D><Object><Key>a&amp;b</Key></Object>"
                       "<Object><Key>c d</Key></Object></D>", "Object", v));
    assert(v.size() == 2);
    std::string k;
    assert(xmlText(v[0], "Key", k) && k == "a&b");
    v.clear();
    assert(!xmlElements("<Object><Key>x</Key>", "Object", v));

    // ---- secret encryption --------------------------------------------------
    auto key = parseMasterKey(std::string(64, 'a'));
    assert(key);
    auto blob = encryptSecret(*key, "s3cr3t-value", "alice");
    assert(blob.find("s3cr3t") == std::string::npos);
    assert(decryptSecret(*key, blob, "alice") == "s3cr3t-value");
    assert(blob != encryptSecret(*key, "s3cr3t-value", "alice")); // fresh nonce
    assert(!decryptSecret(*key, blob, "bob"));                    // bound to the row
    auto other = parseMasterKey("//////////////////////////////////////////8=");
    assert(other && !decryptSecret(*other, blob, "alice"));
    auto tampered = blob;
    tampered[tampered.size() - 6] = tampered[tampered.size() - 6] == 'A' ? 'B' : 'A';
    assert(!decryptSecret(*key, tampered, "alice"));
    assert(!decryptSecret(*key, "", "alice"));
    assert(!decryptSecret(*key, "AAAA", "alice"));
    assert(decryptSecret(*key, encryptSecret(*key, "", "x"), "x") == "");
    assert(!parseMasterKey("short"));
    assert(!parseMasterKey(std::string(64, 'g')));          // not hex, not 32 bytes of base64
    assert(parseMasterKey(std::string(64, 'a') + "\n"));
    std::cout << "misc (Range, ETag, XML, secret crypto): all cases pass\n";
    return 0;
}
