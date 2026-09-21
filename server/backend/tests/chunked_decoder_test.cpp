#include "../src/services/DigestUtil.h"
#include "../src/sigv4/Canonical.h"
#include "../src/sigv4/Checksum.h"
#include "../src/sigv4/ChunkedDecoder.h"
#include "../src/sigv4/PayloadReader.h"

#include <cassert>
#include <iostream>

using namespace s3::sigv4;

namespace {

const char* kSecret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
const char* kDate = "20130524T000000Z";
const char* kScope = "20130524/us-east-1/s3/aws4_request";

ChunkedDecoder::Params params(bool signedChunks, bool trailer,
                              const std::string& seed)
{
    ChunkedDecoder::Params p;
    p.signedChunks = signedChunks;
    p.trailer = trailer;
    p.signingKey = deriveSigningKey(kSecret, "20130524", "us-east-1");
    p.amzDate = kDate;
    p.scope = kScope;
    p.seedSignature = seed;
    return p;
}

std::string sign(const ChunkedDecoder::Params& p, const std::string& prev,
                 const std::string& data)
{
    return signatureHex(p.signingKey,
                        "AWS4-HMAC-SHA256-PAYLOAD\n" + std::string(kDate) + "\n" +
                            kScope + "\n" + prev + "\n" + sha256Hex("") + "\n" +
                            sha256Hex(data));
}

/// A signed aws-chunked body over `data` in chunks of `chunk` bytes.
std::string encodeSigned(const ChunkedDecoder::Params& p, const std::string& data,
                         size_t chunk, const std::string& trailerLine = "")
{
    std::string out, prev = p.seedSignature;
    auto emit = [&](const std::string& d) {
        auto sig = sign(p, prev, d);
        char hex[32];
        std::snprintf(hex, sizeof hex, "%zx", d.size());
        out += std::string(hex) + ";chunk-signature=" + sig + "\r\n" + d +
               (d.empty() ? "" : "\r\n");
        prev = sig;
    };
    for (size_t off = 0; off < data.size(); off += chunk)
        emit(data.substr(off, chunk));
    emit("");
    if (p.trailer) {
        out += trailerLine + "\r\n";
        auto sts = "AWS4-HMAC-SHA256-TRAILER\n" + std::string(kDate) + "\n" +
                   kScope + "\n" + prev + "\n" + sha256Hex(trailerLine + "\n");
        out += "x-amz-trailer-signature:" + signatureHex(p.signingKey, sts) +
               "\r\n";
    }
    return out + "\r\n";
}

struct Decoded {
    ChunkedDecoder::Status status;
    std::string data;
    bool done;
    std::vector<std::pair<std::string, std::string>> trailers;
};

Decoded decode(const ChunkedDecoder::Params& p, const std::string& body,
               size_t slice)
{
    ChunkedDecoder d(p);
    Decoded r{ChunkedDecoder::Status::Ok, "", false, {}};
    for (size_t off = 0; off < body.size(); off += slice) {
        r.status = d.feed(std::string_view(body).substr(off, slice),
                          [&](std::string_view v) { r.data.append(v); });
        if (r.status != ChunkedDecoder::Status::Ok)
            break;
    }
    r.done = d.done();
    r.trailers = d.trailers();
    return r;
}

using S = ChunkedDecoder::Status;

std::string sha256b64(const std::string& s)
{
    Checksummer c(ChecksumAlgo::Sha256);
    c.update(s);
    return c.finish();
}

} // namespace

int chunkedDecoderTest()
{
    // ---- AWS's documented multi-chunk example (65 KiB of 'a', 64 KiB chunks)
    {
        auto p = params(true, false,
                        "4f232c4386841ef735655705268965c44a0e4690baa4adea153f7db9fa80a0a9");
        std::string a1(65536, 'a'), a2(1024, 'a');
        std::string body =
            "10000;chunk-signature=ad80c730a21e5b8d04586a2213dd63b9a0e99e0e2307b0ade35a65485a288648\r\n" +
            a1 + "\r\n" +
            "400;chunk-signature=0055627c9e194cb4542bae2aa5492e3c1575bbb81b612b7d234b86a503ef5497\r\n" +
            a2 + "\r\n" +
            "0;chunk-signature=b6c6ea8a5354eaf15b3cb7646744f4275b71ea724fed81ceb9323e279d449df9\r\n\r\n";
        auto r = decode(p, body, body.size());
        assert(r.status == S::Ok && r.done && r.data == a1 + a2);
        // any slicing, down to a byte at a time, decodes the same
        for (size_t slice : {1u, 7u, 100u, 65536u, 70000u}) {
            auto r2 = decode(p, body, slice);
            assert(r2.status == S::Ok && r2.done && r2.data == a1 + a2);
        }
        // flipping a data byte breaks that chunk's signature
        auto bad = body;
        bad[100] = 'b';
        assert(decode(p, bad, 4096).status == S::BadSignature);
        // wrong seed (request signature) breaks the chain from chunk one
        auto p2 = p;
        p2.seedSignature[0] = p2.seedSignature[0] == '4' ? '5' : '4';
        assert(decode(p2, body, 4096).status == S::BadSignature);
        // chunks dropped or reordered break it too
        auto swapped = "400;chunk-signature=0055627c9e194cb4542bae2aa5492e3c1575bbb81b612b7d234b86a503ef5497\r\n" +
                       a2 + "\r\n";
        assert(decode(p, swapped, 4096).status == S::BadSignature);
        // truncated stream: no error yet, but never done
        auto cut = decode(p, body.substr(0, body.size() - 40), 4096);
        assert(cut.status == S::Ok && !cut.done);
    }

    // ---- signed streams built here: sizes, empty body, trailers ----------
    {
        auto p = params(true, false, std::string(64, 'a'));
        for (size_t n : {0u, 1u, 5u, 1000u}) {
            std::string data(n, 'x');
            auto body = encodeSigned(p, data, 7);
            auto r = decode(p, body, 13);
            assert(r.status == S::Ok && r.done && r.data == data);
        }
        auto ptr = params(true, true, std::string(64, 'b'));
        auto body = encodeSigned(ptr, "hello world", 4,
                                 "x-amz-checksum-crc32:DUoRhQ==");
        auto r = decode(ptr, body, 5);
        assert(r.status == S::Ok && r.done && r.data == "hello world");
        assert(r.trailers.size() == 1 && r.trailers[0].first == "x-amz-checksum-crc32" &&
               r.trailers[0].second == "DUoRhQ==");
        auto tampered = body;
        tampered.replace(tampered.find("DUoRhQ=="), 8, "AAAAAA==");
        assert(decode(ptr, tampered, 9).status == S::BadSignature); // trailer is signed
        // signed trailer stream without the trailer signature
        auto nosig = body.substr(0, body.find("x-amz-trailer-signature")) + "\r\n";
        assert(decode(ptr, nosig, 9).status != S::Ok ||
               !decode(ptr, nosig, 9).done);
    }

    // ---- unsigned + trailer (what SDKs send over TLS) ---------------------
    {
        auto p = params(false, true, "");
        auto r = decode(p, "5\r\nhello\r\n6\r\n world\r\n0\r\nx-amz-checksum-crc32:DUoRhQ==\r\n\r\n", 6);
        assert(r.status == S::Ok && r.done && r.data == "hello world" &&
               r.trailers.size() == 1);
        auto r2 = decode(p, "0\r\n\r\n", 1);
        assert(r2.status == S::Ok && r2.done && r2.data.empty());
        // uppercase hex sizes are fine
        assert(decode(p, "A\r\n0123456789\r\n0\r\n\r\n", 3).data == "0123456789");
    }

    // ---- malformed input -----------------------------------------------
    {
        auto p = params(false, false, "");
        assert(decode(p, "zz\r\nab\r\n0\r\n\r\n", 4).status == S::Malformed);
        assert(decode(p, "\r\n", 4).status == S::Malformed);
        assert(decode(p, "2\r\nabXX0\r\n\r\n", 4).status == S::Malformed);  // no CRLF after data
        assert(decode(p, "2\nab\r\n", 4).status == S::Malformed);           // bare LF header
        assert(decode(p, "12345678901234567\r\n", 4).status == S::Malformed);
        assert(decode(p, std::string(5000, '1'), 100).status == S::Malformed);
        assert(decode(p, "0\r\n\r\njunk", 100).status == S::Malformed);
        assert(!decode(p, "5\r\nhel", 2).done);
        auto ps = params(true, false, std::string(64, 'a'));
        assert(decode(ps, "5\r\nhello\r\n0\r\n\r\n", 4).status == S::Malformed); // signature required
    }

    // ---- checksums ------------------------------------------------------
    {
        auto sum = [](ChecksumAlgo a, const std::string& s) {
            Checksummer c(a);
            c.update(s.substr(0, 3));
            c.update(s.substr(3));
            return c.finish();
        };
        assert(sum(ChecksumAlgo::Crc32, "123456789") == "y/Q5Jg==");     // 0xCBF43926
        assert(sum(ChecksumAlgo::Crc32c, "123456789") == "4waSgw==");    // 0xE3069283
        assert(sum(ChecksumAlgo::Crc64Nvme, "123456789") == "rosUhgp5mIg="); // 0xAE8B14860A799888
        assert(sum(ChecksumAlgo::Crc32, "hello world") == "DUoRhQ==");
        assert(sum(ChecksumAlgo::Sha1, "abc") == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
        assert(sum(ChecksumAlgo::Sha256, "abc") == "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=");
        assert(checksumAlgoFromHeader("x-amz-checksum-crc32c") == ChecksumAlgo::Crc32c);
        assert(!checksumAlgoFromHeader("x-amz-checksum-mode"));
        assert(!checksumAlgoFromHeader("content-md5"));
    }

    // ---- PayloadReader: the whole pipeline ------------------------------
    {
        namespace fs = std::filesystem;
        auto tmp = fs::temp_directory_path() / "s3-payload-test";
        fs::remove_all(tmp);
        std::string data;
        for (int i = 0; i < 300000; ++i)
            data += static_cast<char>('a' + i % 26);
        auto crc = [&] {
            Checksummer c(ChecksumAlgo::Crc32);
            c.update(data);
            return c.finish();
        }();
        auto p = params(true, true, std::string(64, 'c'));
        auto body = encodeSigned(p, data, 65536, "x-amz-checksum-crc32:" + crc);

        auto run = [&](const std::string& hash, const HeaderMap& extra,
                       const std::string& b, size_t memLimit) {
            HeaderMap h = extra;
            PayloadOptions o;
            o.payloadHash = hash;
            o.headers = &h;
            o.tmpDir = tmp;
            o.maxDecodedBytes = 1 << 20;
            o.memoryLimit = memLimit;
            o.chunk = p;
            return preparePayload(b, o);
        };
        HeaderMap h{{"x-amz-decoded-content-length", std::to_string(data.size())},
                    {"x-amz-trailer", "x-amz-checksum-crc32"},
                    {"content-encoding", "aws-chunked,gzip"}};
        auto hash = "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER";
        // in memory, and spooled to a mapped file
        for (size_t mem : {size_t(1) << 20, size_t(1000)}) {
            auto r = run(hash, h, body, mem);
            assert(!r.error && r.payload && r.payload->view == data);
            assert(r.payload->checksum && r.payload->checksum->second == crc);
            assert(r.payload->contentEncoding == "gzip");
        }
        // the spool file is gone once the payload is released
        assert(fs::is_empty(tmp));
        // a wrong trailing checksum, a corrupt chunk, a length lie, no length
        {
            auto wrong = encodeSigned(p, data, 65536, "x-amz-checksum-crc32:AAAAAA==");
            auto r = run(hash, h, wrong, 1000);
            assert(r.error && r.error->code == "BadDigest");
            auto corrupt = body;
            corrupt[200] ^= 1;
            assert(run(hash, h, corrupt, 1000).error->code == "SignatureDoesNotMatch");
            auto h2 = h;
            h2["x-amz-decoded-content-length"] = std::to_string(data.size() - 1);
            assert(run(hash, h2, body, 1000).error->code == "IncompleteBody");
            h2["x-amz-decoded-content-length"] = std::to_string(data.size() + 1);
            assert(run(hash, h2, body, 1000).error->code == "IncompleteBody");
            h2.erase("x-amz-decoded-content-length");
            assert(run(hash, h2, body, 1000).error->status == 411);
            h2["x-amz-decoded-content-length"] = "99999999999";
            assert(run(hash, h2, body, 1000).error->status == 413);
            auto h3 = h;
            h3["x-amz-trailer"] = "x-amz-checksum-sha256"; // declared, never sent
            assert(run(hash, h3, body, 1000).error->code == "InvalidRequest");
            assert(fs::is_empty(tmp)); // failures leave no spool files
        }
        // plain bodies: sha256, md5, checksum header, unsigned
        {
            std::string plain = "hello world";
            HeaderMap ph;
            auto r = run(sha256Hex(plain), ph, plain, 100);
            assert(!r.error && r.payload->view.data() == plain.data()); // in place
            assert(run(sha256Hex("other"), ph, plain, 100).error->code ==
                   "XAmzContentSHA256Mismatch");
            assert(!run("UNSIGNED-PAYLOAD", ph, plain, 100).error);
            assert(run("bogus", ph, plain, 100).error->code == "InvalidRequest");
            auto md = ph;
            md["content-md5"] = "XrY7u+Ae7tCTyyK7j1rNww==";
            assert(!run("UNSIGNED-PAYLOAD", md, plain, 100).error);
            assert(run("UNSIGNED-PAYLOAD", md, "hello worle", 100).error->code == "BadDigest");
            md["content-md5"] = "not base64!";
            assert(run("UNSIGNED-PAYLOAD", md, plain, 100).error->code == "InvalidDigest");
            auto ck = ph;
            ck["x-amz-checksum-crc32"] = "DUoRhQ==";
            ck["x-amz-checksum-mode"] = "ENABLED";
            auto rc = run("UNSIGNED-PAYLOAD", ck, plain, 100);
            assert(!rc.error && rc.payload->checksum->first == "x-amz-checksum-crc32");
            ck["x-amz-checksum-crc32"] = "AAAAAA==";
            assert(run("UNSIGNED-PAYLOAD", ck, plain, 100).error->code == "BadDigest");
            HeaderMap h4 = ck;
            PayloadOptions o;
            o.payloadHash = "UNSIGNED-PAYLOAD";
            o.headers = &h4;
            o.tmpDir = tmp;
            o.checkChecksumHeaders = false; // POST: describes the object, not the XML
            assert(!preparePayload(plain, o).error);
        }
        fs::remove_all(tmp);
    }
    std::cout << "ChunkedDecoder/PayloadReader: all cases pass\n";
    return 0;
}
