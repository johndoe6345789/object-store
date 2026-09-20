# object-store

Self-contained S3-compatible object store.

Path-style URLs (`/{bucket}/{key}`) and a small credential scheme,
`Authorization: AWS <access_key>:<secret_key>`, checked against the `api_keys`
table. This is not AWS Signature V4, so AWS SDKs do not talk to it unchanged.

    S3_DB_CONN   libpq connection string (also PGHOST/PGUSER/... for migrations)
    S3_DATA_DIR  blob root, default /data/s3
    S3_PORT      listen port, default 9000
    S3_REGION    reported region, default us-east-1

## Running the tests

Unit tests are plain executables with asserts; the image build runs them:

    cmake -B out -DCMAKE_BUILD_TYPE=Release
    cmake --build out --target s3tests && ./out/s3tests

## Two rules worth keeping

**Handlers must not run on drogon's IO loops.** Every store call here is
synchronous (`execSqlSync`, filesystem reads and writes). A loop thread
blocked in one of them stops serving *every* connection the kernel hands it
afterwards, so the server answers some requests in milliseconds and silently
never answers others -- clients just hang until their own timeout, and
`/health` looks intermittently dead too. Handlers hand their work to
`Workers` (see `services/Workers.h`, `services/OffLoop.h`) and reply from
there, which drogon allows from any thread.

**Blobs are content-addressed, so a file can have more than one owner.**
`bucket/<md5>` means two keys with identical bytes share one file. Deleting a
key must therefore check `ObjectStore::pathInUse()` before unlinking, or the
surviving key answers 200 with an empty body.

## Default credential

`migrations/002_seed_data.sql` seeds `minioadmin/minioadmin` with `admin`
permissions, but only while the `api_keys` table is empty: the entrypoint
re-runs every migration on each start, and an unconditional insert used to
resurrect that key after a deployment had deleted it. Replace it with a real
key on anything reachable by others.

---
Split out of `metabuilder/services/object-store` as part of the [reposplit](https://github.com/johndoe6345789/reposplit) effort.
