#include "../src/services/BlobStore.h"
#include "../src/services/MultipartStore.h"
#include <cassert>
#include <iostream>

/**
 * Multipart logic on a scratch directory: parts overwrite idempotently, the
 * completion plan enforces order/existence/size, assembly streams and hashes
 * correctly, and the sweep removes only old, idle uploads.
 */
int multipartTest()
{
    namespace fs = std::filesystem;
    using namespace s3;
    auto root = fs::temp_directory_path() / "s3-multipart-test";
    fs::remove_all(root);
    MultipartStore mp(root);
    const uintmax_t kMax = 1000;

    auto id = mp.create({"alice", "bkt", "big/object.bin", "text/plain"});
    assert(isValidUploadId(id));
    assert(id != mp.create({"alice", "bkt", "big/object.bin", ""})); // unique
    auto meta = mp.load(id);
    assert(meta && meta->owner == "alice" && meta->bucket == "bkt" &&
           meta->key == "big/object.bin" && meta->contentType == "text/plain");
    assert(!mp.load("../etc"));
    assert(!mp.load(std::string(32, 'f'))); // valid shape, unknown upload

    // Parts land at .uploads/<id>/<n>; etag is the part's md5; re-send wins.
    auto r2 = mp.putPart(id, 2, "world", kMax);
    assert(r2.error.empty() && r2.etag == md5hex("world"));
    assert(fs::exists(root / ".uploads" / id / "2"));
    assert(mp.putPart(id, 1, "old", kMax).error.empty());
    assert(mp.putPart(id, 1, "hello ", kMax).error.empty()); // retry overwrites
    assert(!mp.putPart(id, 0, "x", kMax).error.empty());
    assert(!mp.putPart(id, 10001, "x", kMax).error.empty());
    assert(!mp.putPart("nothex", 1, "x", kMax).error.empty());
    auto stored = mp.listParts(id);
    assert(stored.size() == 2 && stored[0].number == 1 &&
           stored[0].size == 6 && stored[1].number == 2);
    for (auto& e : fs::directory_iterator(root / ".uploads" / id))
        assert(e.path().filename().string().find("tmp.") != 0); // no strays

    // Plan: empty request = all parts ascending; explicit must be ordered.
    auto all = MultipartStore::plan(stored, {}, kMax);
    assert(all.error.empty() && all.parts.size() == 2 && all.total == 11);
    assert(MultipartStore::plan(stored, {1, 2}, kMax).error.empty());
    assert(MultipartStore::plan(stored, {2, 1}, kMax).error == "InvalidPartOrder");
    assert(MultipartStore::plan(stored, {1, 1}, kMax).error == "InvalidPartOrder");
    assert(MultipartStore::plan(stored, {1, 3}, kMax).error == "InvalidPart");
    assert(MultipartStore::plan({}, {}, kMax).error == "MalformedXML");
    // Size cap: whole-object total, and at part time too.
    assert(MultipartStore::plan(stored, {}, 10).error == "EntityTooLarge");
    assert(mp.putPart(id, 3, std::string(990, 'x'), kMax).error == "EntityTooLarge");
    assert(mp.putPart(id, 2, std::string(990, 'x'), kMax).error.empty()); // replaces 5B
    assert(mp.putPart(id, 2, "world", kMax).error.empty());

    // Assembly: concatenation in plan order, md5 of the whole, streamed.
    auto out = root / "assembled";
    auto a = mp.assemble(id, all.parts, out);
    assert(a.size == 11 && a.etag == md5hex("hello world"));
    std::ifstream in(out, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), {});
    assert(got == "hello world");
    auto only2 = MultipartStore::plan(stored, {2}, kMax);
    assert(mp.assemble(id, only2.parts, out).etag == md5hex("world"));

    // Larger than the 1 MiB buffer, to exercise the chunk loop.
    auto big = mp.create({"alice", "bkt", "k", ""});
    std::string chunk(3 * 1024 * 1024 + 17, 'z');
    assert(mp.putPart(big, 1, chunk, 1ULL << 30).error.empty());
    assert(mp.putPart(big, 2, chunk, 1ULL << 30).error.empty());
    auto bp = MultipartStore::plan(mp.listParts(big), {}, 1ULL << 30);
    auto ba = mp.assemble(big, bp.parts, out);
    assert(ba.size == 2 * chunk.size() && ba.etag == md5hex(chunk + chunk));

    // Locks: parts share, complete/abort/sweep are exclusive and refuse.
    {
        auto s1 = mp.lock(id, false);
        auto s2 = mp.lock(id, false);
        assert(s1 && s2 && !mp.lock(id, true));
    }
    {
        auto x = mp.lock(id, true);
        assert(x && !mp.lock(id, false) && !mp.lock(id, true));
    }
    assert(mp.lock(id, true)); // released again

    // Sweep: only ids older than the cutoff go, and never a locked one.
    auto oldId = mp.create({"bob", "bkt", "old", ""});
    auto busyId = mp.create({"bob", "bkt", "busy", ""});
    auto past = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(root / ".uploads" / oldId, past);
    fs::last_write_time(root / ".uploads" / busyId, past);
    fs::create_directories(root / ".uploads" / "not-an-id");
    fs::last_write_time(root / ".uploads" / "not-an-id", past);
    {
        auto g = mp.lock(busyId, false);
        assert(mp.sweep(std::chrono::hours(24)) == 1);
    }
    assert(!fs::exists(root / ".uploads" / oldId));
    assert(fs::exists(root / ".uploads" / busyId));
    assert(fs::exists(root / ".uploads" / id));          // fresh: kept
    assert(fs::exists(root / ".uploads" / "not-an-id")); // not ours: kept
    assert(mp.sweep(std::chrono::hours(24)) == 1);       // busy one now idle
    mp.remove(id);
    assert(!fs::exists(root / ".uploads" / id));

    // Staged blobs: a discarded staging leaves nothing, publish is by md5.
    BlobStore blobs(root / "blobs");
    {
        auto s = blobs.stage("bkt", "abc");
        assert(fs::exists(s.tmp));
    }
    auto s = blobs.stage("bkt", "abc");
    assert(s.etag == md5hex("abc") && s.size == 3);
    assert(blobs.publish("bkt", s) == "bkt/" + md5hex("abc"));
    assert(*blobs.read("bkt/" + md5hex("abc")) == "abc");
    int strays = 0;
    for (auto& e : fs::recursive_directory_iterator(root / "blobs"))
        strays += e.path().filename().string().find(".tmp.") == 0;
    assert(strays == 0);
    blobs.removeBucket("bkt");
    assert(!blobs.read("bkt/" + md5hex("abc")));
    blobs.removeBucket(".."); // refused, must not touch anything
    assert(fs::exists(root));

    fs::remove_all(root);
    std::cout << "Multipart: all cases pass\n";
    return 0;
}
