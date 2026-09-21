#include "../src/sigv4/Canonical.h"
#include "../src/sigv4/SigV4Verify.h"
#include "../src/sigv4/UriEncoding.h"

#include <cassert>
#include <iostream>

using namespace s3::sigv4;

namespace {

const char* kSecret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
const char* kEmptySha =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

/// Verify a header-auth request the way AuthFilter does.
VerifyOutcome check(const std::string& method, const std::string& path,
                    const std::string& query, const HeaderMap& h,
                    const std::string& authorization, time_t now,
                    const VerifyConfig& cfg = {})
{
    ParsedAuth p;
    auto e = parseAuthorizationHeader(authorization, p);
    if (e) {
        VerifyOutcome o;
        o.error = e;
        return o;
    }
    VerifyInput in;
    in.method = method;
    in.rawPath = path;
    in.rawQuery = query;
    in.headers = &h;
    in.now = now;
    return verify(p, kSecret, in, cfg);
}

const time_t kNow = 1369353600 + 30; // 20130524T000030Z

std::string auth(const std::string& signedHeaders, const std::string& sig)
{
    return "AWS4-HMAC-SHA256 "
           "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,"
           "SignedHeaders=" + signedHeaders + ",Signature=" + sig;
}

} // namespace

/// AWS's published S3 SigV4 examples (docs: "Signature Calculations for the
/// Authorization Header"), plus the failure modes around them.
int sigv4Test()
{
    // ---- the four header-auth examples ---------------------------------
    {
        HeaderMap h{{"host", "examplebucket.s3.amazonaws.com"},
                    {"range", "bytes=0-9"},
                    {"x-amz-content-sha256", kEmptySha},
                    {"x-amz-date", "20130524T000000Z"}};
        auto a = auth("host;range;x-amz-content-sha256;x-amz-date",
                      "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
        auto o = check("GET", "/test.txt", "", h, a, kNow);
        assert(!o.error); // GET Object

        auto bad = h; // a tampered signed header
        bad["range"] = "bytes=0-99";
        auto ob = check("GET", "/test.txt", "", bad, a, kNow);
        assert(ob.error && ob.error->code == "SignatureDoesNotMatch");

        auto missing = h; // a signed header that is absent
        missing.erase("range");
        assert(check("GET", "/test.txt", "", missing, a, kNow).error->code ==
               "SignatureDoesNotMatch");

        assert(check("GET", "/test.txt", "", h,
                     a.substr(0, a.size() - 1) + "0", kNow).error->code ==
               "SignatureDoesNotMatch");
        assert(check("GET", "/test2.txt", "", h, a, kNow).error->code ==
               "SignatureDoesNotMatch");
        assert(check("HEAD", "/test.txt", "", h, a, kNow).error->code ==
               "SignatureDoesNotMatch");
    }
    {
        HeaderMap h{{"date", "Fri, 24 May 2013 00:00:00 GMT"},
                    {"host", "examplebucket.s3.amazonaws.com"},
                    {"x-amz-content-sha256",
                     "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"},
                    {"x-amz-date", "20130524T000000Z"},
                    {"x-amz-storage-class", "REDUCED_REDUNDANCY"}};
        auto a = auth("date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class",
                      "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
        // The key contains '$', which must be encoded once as %24.
        assert(!check("PUT", "/test%24file.text", "", h, a, kNow).error);
        assert(!check("PUT", "/test$file.text", "", h, a, kNow).error); // sent unencoded
    }
    {
        HeaderMap h{{"host", "examplebucket.s3.amazonaws.com"},
                    {"x-amz-content-sha256", kEmptySha},
                    {"x-amz-date", "20130524T000000Z"}};
        auto a = auth("host;x-amz-content-sha256;x-amz-date",
                      "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");
        assert(!check("GET", "/", "lifecycle", h, a, kNow).error); // GET lifecycle
        assert(!check("GET", "/", "lifecycle=", h, a, kNow).error);

        auto a2 = auth("host;x-amz-content-sha256;x-amz-date",
                       "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");
        assert(!check("GET", "/", "max-keys=2&prefix=J", h, a2, kNow).error);
        // Parameter order on the wire does not matter, encoding of the value does not either.
        assert(!check("GET", "/", "prefix=J&max-keys=2", h, a2, kNow).error);
        assert(!check("GET", "/", "prefix=%4A&max-keys=2", h, a2, kNow).error);
        assert(check("GET", "/", "prefix=K&max-keys=2", h, a2, kNow).error);
    }

    // ---- presigned URL example -----------------------------------------
    {
        const std::string q =
            "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential="
            "AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
            "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400"
            "&X-Amz-SignedHeaders=host&X-Amz-Signature="
            "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404";
        assert(isPresignedQuery(q));
        assert(!isPresignedQuery("prefix=a"));
        HeaderMap h{{"host", "examplebucket.s3.amazonaws.com"}};
        auto run = [&](time_t now, const std::string& query) {
            ParsedAuth p;
            auto e = parsePresignedQuery(query, p);
            VerifyOutcome o;
            if (e) {
                o.error = e;
                return o;
            }
            VerifyInput in;
            in.method = "GET";
            in.rawPath = "/test.txt";
            in.rawQuery = query;
            in.headers = &h;
            in.now = now;
            return verify(p, kSecret, in, {});
        };
        assert(!run(kNow, q).error);
        assert(!run(1369353600 + 86400, q).error);            // last second
        auto ex = run(1369353600 + 86401, q);                 // expired
        assert(ex.error && ex.error->code == "AccessDenied" &&
               ex.error->message == "Request has expired");
        assert(run(1369353600 - 3600, q).error->code == "AccessDenied");  // not yet valid
        auto tampered = q;
        tampered.replace(tampered.find("86400"), 5, "86401"); // signed parameter
        assert(run(kNow, tampered).error->code == "SignatureDoesNotMatch");
        auto week = q;
        week.replace(week.find("86400"), 5, "604801");
        assert(run(kNow, week).error->code == "AuthorizationQueryParametersError");
        auto zero = q;
        zero.replace(zero.find("86400"), 5, "0");
        assert(run(kNow, zero).error->code == "AuthorizationQueryParametersError");
        // Signature parameter order/position does not change what is signed.
        auto reordered = "X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404&" +
                         q.substr(0, q.find("&X-Amz-Signature="));
        assert(!run(kNow, reordered).error);
    }

    // ---- policy: clock, region, scheme --------------------------------
    {
        HeaderMap h{{"host", "examplebucket.s3.amazonaws.com"},
                    {"range", "bytes=0-9"},
                    {"x-amz-content-sha256", kEmptySha},
                    {"x-amz-date", "20130524T000000Z"}};
        auto a = auth("host;range;x-amz-content-sha256;x-amz-date",
                      "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
        auto skew = check("GET", "/test.txt", "", h, a, 1369353600 + 901);
        assert(skew.error && skew.error->code == "RequestTimeTooSkewed");
        assert(!check("GET", "/test.txt", "", h, a, 1369353600 + 899).error);
        assert(check("GET", "/test.txt", "", h, a, kNow - 1000).error->code ==
               "RequestTimeTooSkewed");
        VerifyConfig wide;
        wide.maxSkewSeconds = 7200;
        assert(!check("GET", "/test.txt", "", h, a, kNow + 1000, wide).error);

        VerifyConfig eu;
        eu.region = "eu-west-1";
        auto wrong = check("GET", "/test.txt", "", h, a, kNow, eu);
        assert(wrong.error && wrong.error->code == "AuthorizationHeaderMalformed");
        eu.anyRegion = true; // S3_ANY_REGION=1
        assert(!check("GET", "/test.txt", "", h, a, kNow, eu).error);

        // the legacy scheme, other schemes, and mangled headers
        ParsedAuth p;
        assert(parseAuthorizationHeader("AWS alice:secret", p)->code ==
               "AuthorizationHeaderMalformed");
        assert(parseAuthorizationHeader("Bearer abc", p));
        assert(parseAuthorizationHeader("AWS4-HMAC-SHA256 Credential=x", p));
        assert(parseAuthorizationHeader(
            "AWS4-HMAC-SHA256 Credential=AK/20130524/us-east-1/ec2/aws4_request,"
            "SignedHeaders=host,Signature=" + std::string(64, 'a'), p));
        assert(parseAuthorizationHeader(
            "AWS4-HMAC-SHA256 Credential=AK/20130524/us-east-1/s3/aws4_request,"
            "SignedHeaders=host,Signature=zz", p));
        // spaces after commas, as most SDKs write it
        assert(!parseAuthorizationHeader(
            "AWS4-HMAC-SHA256 Credential=AK/20130524/us-east-1/s3/aws4_request, "
            "SignedHeaders=host;x-amz-date, Signature=" + std::string(64, 'a'), p));
        assert(p.accessKey == "AK" && p.signedHeaders.size() == 2);

        // x-amz-* headers the store acts on must be covered by the signature
        auto extra = h;
        extra["x-amz-copy-source"] = "/b/k";
        assert(check("GET", "/test.txt", "", extra, a, kNow).error->code ==
               "AccessDenied");
        // secrets are never part of an error
        assert(skew.error->message.find(kSecret) == std::string::npos);
    }

    // ---- canonicalisation details --------------------------------------
    {
        assert(canonicalUri("") == "/");
        assert(canonicalUri("/b/a%20b") == "/b/a%20b");
        assert(canonicalUri("/b/a+b") == "/b/a%2Bb");     // '+' in a path is a plus
        assert(canonicalUri("/b/a%2Bb") == "/b/a%2Bb");
        assert(canonicalUri("/b/%C3%A9") == "/b/%C3%A9"); // encoded once, not twice
        assert(canonicalUri("/b/\xC3\xA9") == "/b/%C3%A9");
        assert(canonicalUri("/b/k(1)!*'~") == "/b/k%281%29%21%2A%27~");
        assert(canonicalUri("/b/dir/") == "/b/dir/");
        assert(canonicalUri("/b//x") == "/b//x");         // S3 does not merge slashes
        assert(canonicalUri("/b/100%") == "/b/100%25");   // stray '%'
        assert(canonicalQuery("b=2&a=1&a=0&c", false) == "a=0&a=1&b=2&c=");
        assert(canonicalQuery("prefix=a+b&x=%2F", false) == "prefix=a%20b&x=%2F");
        assert(canonicalQuery("X-Amz-Signature=abc&a=1", true) == "a=1");
        assert(canonicalQuery("X-Amz-Signature=abc&a=1", false) ==
               "X-Amz-Signature=abc&a=1");
        assert(canonicalQuery("", false).empty());
        assert(canonicalQuery("list-type=2&continuation-token=1%2B%3D", false) ==
               "continuation-token=1%2B%3D&list-type=2");
        assert(trimHeaderValue("  a   b \t c  ") == "a b c");
        assert(uriDecode("a%2", false) == "a%2");
        assert(uriDecode("a+b%20c", true) == "a b c");
        assert(uriDecode("a+b", false) == "a+b");
    }
    std::cout << "SigV4: all cases pass\n";
    return 0;
}
