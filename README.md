# object-store

Self-contained S3-compatible object store.

Path-style URLs (`/{bucket}/{key}`) and a small credential scheme,
`Authorization: AWS <access_key>:<secret_key>`, checked against the `api_keys`
table. This is not AWS Signature V4, so AWS SDKs do not talk to it unchanged.

    S3_DB_CONN   libpq connection string (also PGHOST/PGUSER/... for migrations)
    S3_DATA_DIR  blob root, default /data/s3
    S3_PORT      listen port, default 9000
    S3_REGION    reported region, default us-east-1
    S3_MAX_OBJECT_BYTES  cap on one object (single PUT or completed multipart
                 upload), default 2147483648 (2 GiB); larger is 413 EntityTooLarge

## Running the tests

Unit tests are plain executables with asserts; the image build runs them:

    cmake -B out -DCMAKE_BUILD_TYPE=Release
    cmake --build out --target s3tests && ./out/s3tests

`server/backend/tests/integration/run.sh` drives a built image against a
throwaway postgres (auth, cross-owner isolation, multipart, size cap); CI runs
it after the image build:

    docker build -t s3server:test -f server/backend/Dockerfile server
    IMAGE=s3server:test server/backend/tests/integration/run.sh

The admin UI (`server/frontend`) builds on its own:
`npm ci && npm run lint && npm run typecheck && npm test && npm run build`,
or `docker build server/frontend`. The image proxies `/api/s3/*` to
`S3_BACKEND_URL` (default `http://backend:9000`), read at runtime; the login
secret is held in memory for the tab only, never in web storage.

## Permission model

Every request except `GET /health` needs `Authorization: AWS <access_key>:<secret_key>`.
A key row carries an `owner` and a `permissions` list; the list is split on
commas and whitespace and matched as exact tokens (`readonly` does not grant
`read`):

| token   | grants                                              |
|---------|-----------------------------------------------------|
| `read`  | GET / HEAD (list, get object, head bucket/object)   |
| `write` | everything else (create/delete bucket, put/delete object, multipart) |
| `admin` | both of the above                                   |

Buckets are scoped to the key's `owner`: another owner's bucket, object or
upload behaves as if it does not exist (404), whichever route asks, and
`admin` does not cross owners. A bucket is always created for the calling
key's own owner. Bucket names are globally unique (a taken name is 409 for
everyone) and limited to `[A-Za-z0-9._-]`, no leading dot, no `..`. A wrong
secret and an unknown access key get the identical 403 response.

Least-privilege key (read-only, for one tenant):

    INSERT INTO api_keys (access_key, secret_key, owner, permissions)
    VALUES ('tenant-a-ro', encode(gen_random_bytes(24), 'hex'), 'tenant-a', 'read');
    -- read/write app key: permissions 'read,write'.  Show the new secret once:
    SELECT secret_key FROM api_keys WHERE access_key = 'tenant-a-ro';

(`gen_random_bytes` needs `CREATE EXTENSION pgcrypto;`; any random string works.)
Secrets are stored and compared as-is (constant-time), and are never logged.

## Multipart upload

For objects too big for one request (parts arrive through a proxy with a
100 MB body cap). The object is never held in memory: parts are spooled to
`$S3_DATA_DIR/.uploads/<uploadId>/<partNumber>` and concatenated to the blob
by streaming, hashing as it goes. Uploads idle for 24 h are swept (at start,
then hourly).

    AUTH="Authorization: AWS ACCESS:SECRET"; U=http://localhost:9000

    # 1. initiate -> <UploadId>...</UploadId> (256-bit random hex)
    curl -X POST -H "$AUTH" "$U/bucket/big.iso?uploads"

    # 2. upload parts 1..10000, each up to 100 MB; re-sending a part overwrites it
    split -b 50M big.iso part.
    n=1; for f in part.*; do
      curl -X PUT -H "$AUTH" --data-binary @$f -D - \
        "$U/bucket/big.iso?partNumber=$n&uploadId=$ID"; n=$((n+1)); done

    # 3. complete: body optional (empty = all parts ascending); else S3 XML
    curl -X POST -H "$AUTH" "$U/bucket/big.iso?uploadId=$ID" \
      -d '<CompleteMultipartUpload><Part><PartNumber>1</PartNumber></Part>...</CompleteMultipartUpload>'

    # or abort (204): drops the parts
    curl -X DELETE -H "$AUTH" "$U/bucket/big.iso?uploadId=$ID"

Part responses carry `ETag: "<md5 of the part>"`. Complete answers
`<CompleteMultipartUploadResult>` with `ETag` = md5 of the whole object (the
same value a single PUT of those bytes gets, not S3's md5-of-md5s). ETags in
the completion body are not checked; part numbers must be ascending and exist
(`InvalidPartOrder` / `InvalidPart`, 400). An upload belongs to the
(owner, bucket, key) that started it; anyone else, or a different bucket/key,
gets `NoSuchUpload` 404. Errors: 413 `EntityTooLarge` (part over 100 MB, or
total over `S3_MAX_OBJECT_BYTES`), 409 `OperationAborted` (another request is
completing/aborting the same upload). Keys may contain `/`.

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
