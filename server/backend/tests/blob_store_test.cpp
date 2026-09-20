#include "../src/services/BlobStore.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

/**
 * Blobs are content-addressed (bucket/<md5>), which means two keys holding
 * the same bytes are one file on disk. These are the cases that got that
 * wrong: a delete that took the survivor's data with it, and a missing blob
 * reported as an empty object.
 */
int blobStoreTest()
{
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "s3-blobstore-test";
    fs::remove_all(root);
    s3::BlobStore blobs(root);

    // Same bytes under two keys: one file, one etag.
    auto a = blobs.store("bucket", "key-a", "identical payload");
    auto b = blobs.store("bucket", "key-b", "identical payload");
    assert(a.etag == b.etag);
    assert(a.path == b.path);
    assert(a.size == 17);

    // Different bytes: different file.
    auto c = blobs.store("bucket", "key-c", "something else");
    assert(c.path != a.path);

    // Round-trip.
    auto read = blobs.read(a.path);
    assert(read.has_value());
    assert(*read == "identical payload");

    // Deleting the shared file is what the caller must avoid while another
    // key still points at it; once removed, a read says so rather than
    // handing back an empty string that looks like a zero-length object.
    assert(blobs.remove(a.path));
    assert(!blobs.read(a.path).has_value());
    assert(blobs.read(c.path).has_value());

    // A path that was never stored is not an empty object either.
    assert(!blobs.read("bucket/does-not-exist").has_value());

    // No temporary files left behind by a successful write.
    int strays = 0;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.path().string().find(".tmp.") != std::string::npos)
            ++strays;
    assert(strays == 0);

    fs::remove_all(root);
    std::cout << "BlobStore: all cases pass\n";
    return 0;
}
