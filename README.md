# object-store

Self-contained S3-compatible object store. Standard S3 clients and SDKs (aws-cli
v2, boto3, `@aws-sdk/client-s3` v3, rclone, minio-go/mc, ...) work against it
unchanged.

- **Authentication is AWS Signature Version 4 only**: the `Authorization:
  AWS4-HMAC-SHA256 ...` header and presigned URLs. The former private
  `Authorization: AWS <access>:<secret>` scheme is gone; such a request is
  refused (403 `AuthorizationHeaderMalformed`).
- **Path-style addressing** (`https://host/bucket/key`), region `us-east-1`.
- Bodies are stored as content-addressed blobs on disk, metadata in PostgreSQL.

Configuration (environment):

    S3_DB_CONN   libpq connection string (also PGHOST/PGUSER/... for migrations)
    S3_DATA_DIR  blob root, default /data/s3
    S3_PORT      listen port, default 9000
    S3_REGION    the region clients must sign for, default us-east-1
    S3_ANY_REGION=1   accept a request signed for any region (lenient mode)
    S3_CLOCK_SKEW_SECONDS  allowed |client clock - server clock|, default 900
    S3_MAX_OBJECT_BYTES  cap on one object (single PUT or completed multipart
                 upload), default 2147483648 (2 GiB); larger is 413 EntityTooLarge
    S3_SECRET_ENCRYPTION_KEY  master key for encrypting key secrets at rest
                 (see "Minting keys and secret encryption"); unset = plaintext
                 and a warning at start
    S3_SECRET_ENCRYPTION_KEY_PREVIOUS  the old master key while rotating it

Migrations (`server/backend/migrations/*.sql`) are re-run by the entrypoint on
every start, so they are all idempotent.

## Using standard S3 clients

Point the client at `https://<host>` (your reverse proxy in front of port 9000)
with **path-style** addressing and region `us-east-1`. Nothing else is needed:
the default checksum behaviour of current SDKs (`aws-chunked` uploads with a
trailing CRC) works, so `AWS_REQUEST_CHECKSUM_CALCULATION=when_required` is
**not** required. Virtual-hosted-style (`bucket.host`) is not supported (it
needs wildcard DNS and a wildcard certificate); the clients below are all told
to use path-style.

The reverse proxy must forward the request's `Host` header unchanged (nginx:
`proxy_set_header Host $http_host;`, CapRover does this by default), and must not
buffer or cap large bodies (`client_max_body_size 0`).

**aws-cli v2**

    export AWS_ACCESS_KEY_ID=<access> AWS_SECRET_ACCESS_KEY=<secret>
    export AWS_DEFAULT_REGION=us-east-1
    aws configure set default.s3.addressing_style path
    aws --endpoint-url https://s3.example.com s3 mb s3://photos
    aws --endpoint-url https://s3.example.com s3 cp big.iso s3://photos/    # multipart
    aws --endpoint-url https://s3.example.com s3 sync ./site s3://photos/site/
    aws --endpoint-url https://s3.example.com s3 ls s3://photos/site/ --recursive
    aws --endpoint-url https://s3.example.com s3 presign s3://photos/big.iso --expires-in 3600
    aws --endpoint-url https://s3.example.com s3 rm s3://photos --recursive

**boto3**

    import boto3
    from botocore.client import Config
    s3 = boto3.client(
        "s3", endpoint_url="https://s3.example.com", region_name="us-east-1",
        aws_access_key_id="<access>", aws_secret_access_key="<secret>",
        config=Config(signature_version="s3v4", s3={"addressing_style": "path"}))
    s3.upload_file("big.iso", "photos", "big.iso")           # TransferManager, multipart
    url = s3.generate_presigned_url("get_object", ExpiresIn=3600,
                                    Params={"Bucket": "photos", "Key": "big.iso"})

(`signature_version="s3v4"` matters for *presigned* URLs: boto3's default for
`generate_presigned_url` on a custom endpoint is the older SigV2 query scheme,
which this store does not accept.)

**JavaScript (`@aws-sdk/client-s3` v3)**

    import { S3Client, PutObjectCommand } from "@aws-sdk/client-s3";
    const s3 = new S3Client({
      endpoint: "https://s3.example.com", region: "us-east-1",
      forcePathStyle: true,
      credentials: { accessKeyId: "<access>", secretAccessKey: "<secret>" },
    });
    await s3.send(new PutObjectCommand({ Bucket: "photos", Key: "a.txt", Body: "hi" }));
    // multipart: `new Upload({ client: s3, params: {...} })` from @aws-sdk/lib-storage

**rclone** (`~/.config/rclone/rclone.conf`)

    [store]
    type = s3
    provider = Other
    endpoint = https://s3.example.com
    access_key_id = <access>
    secret_access_key = <secret>
    region = us-east-1
    force_path_style = true

    rclone copy ./dir store:photos/dir
    rclone ls store:photos

**mc / minio-go** (not part of the automated suite): `mc alias set store https://s3.example.com <access> <secret> --api S3v4 --path on`.

**curl** (7.75+): `curl --aws-sigv4 "aws:amz:us-east-1:s3" --user "<access>:<secret>" https://s3.example.com/photos/a.txt`

### Presigned URLs

Query-string authentication (`X-Amz-Algorithm`, `X-Amz-Credential`, `X-Amz-Date`,
`X-Amz-Expires`, `X-Amz-SignedHeaders`, `X-Amz-Signature`) works for GET, HEAD and
PUT, with `X-Amz-Expires` from 1 second up to 7 days. An expired URL is 403
`AccessDenied` ("Request has expired"); a longer lifetime is 400
`AuthorizationQueryParametersError`. A presigned GET can be opened straight in a
browser (`response-content-type` / `response-content-disposition` query
overrides are honoured, e.g. to force a download name). A presigned URL carries
the permissions of the key that made it, and only reaches that key's owner's
buckets.

## Permission model

Every request except `GET /health` must be SigV4-signed by an `api_keys` row.
A key row carries an `owner` and a `permissions` list; the list is split on
commas and whitespace and matched as exact tokens (`readonly` does not grant
`read`):

| token   | grants                                              |
|---------|-----------------------------------------------------|
| `read`  | GET / HEAD (list, get object, head bucket/object, list parts/uploads) |
| `write` | everything else (create/delete bucket, put/copy/delete object, DeleteObjects, multipart) |
| `admin` | both of the above                                   |

CopyObject / UploadPartCopy additionally need `read` (they read the source).
Permissions and owner scoping are identical for header-signed and presigned
requests.

Buckets are scoped to the key's `owner`: another owner's bucket, object or
upload behaves as if it does not exist (404), whichever route asks, and
`admin` does not cross owners. A bucket is always created for the calling
key's own owner. Bucket names are globally unique (a taken name is 409 for
everyone) and limited to `[A-Za-z0-9._-]`, no leading dot, no `..`.

A signature is checked before anything else is revealed; like S3, an unknown
access key is `InvalidAccessKeyId` and a wrong secret `SignatureDoesNotMatch`.
Errors are S3 XML (`<Error><Code/><Message/><RequestId/><HostId/></Error>`) and
every response carries `x-amz-request-id`.

## Minting keys and secret encryption

Create a least-privilege key (read-only, for one tenant) with SQL, using a
random secret, and show it once:

    INSERT INTO api_keys (access_key, secret_key, owner, permissions)
    VALUES ('tenant-a-ro', encode(gen_random_bytes(24), 'hex'), 'tenant-a', 'read');
    SELECT secret_key FROM api_keys WHERE access_key = 'tenant-a-ro';

(`gen_random_bytes` needs `CREATE EXTENSION pgcrypto;`; any random string
works. Use `'read,write'` for an application key.)

SigV4 signing needs the plaintext secret, so secrets cannot be hashed. To keep
them off the disk in the clear, set **`S3_SECRET_ENCRYPTION_KEY`** to a random
32-byte master key (64 hex characters, or base64 of 32 bytes; e.g.
`openssl rand -hex 32`). Each secret is then AES-256-GCM encrypted (bound to its
access key) the first time that key is used: `api_keys.secret_enc` holds the
ciphertext and `api_keys.secret_key` becomes the marker `!enc`. Notes:

- Keep the master key somewhere other than the database backup; without it the
  encrypted secrets are unrecoverable (issue new keys). Take a DB backup before
  first enabling it.
- Without the variable the store keeps plaintext secrets and logs a warning at
  start. If the variable is later removed, keys already encrypted fail closed
  (500 `InternalError`, logged) until it is set again, or the key's
  `secret_key` is reset to a plaintext value.
- **Rotating a key's secret**: `UPDATE api_keys SET secret_key = '<new secret>'
  WHERE access_key = '...'`. A plaintext `secret_key` always wins over
  `secret_enc`, and is re-encrypted on its next use.
- **Rotating the master key**: restart with the new value in
  `S3_SECRET_ENCRYPTION_KEY` and the old one in
  `S3_SECRET_ENCRYPTION_KEY_PREVIOUS`; each row migrates on its next use. Drop
  `_PREVIOUS` once every row you still need has been used (or reset it).
- Secrets are compared in constant time and never logged.

## Multipart upload

Standard multipart works from every SDK (`CreateMultipartUpload`, `UploadPart`,
`UploadPartCopy`, `CompleteMultipartUpload`, `AbortMultipartUpload`, `ListParts`,
`ListMultipartUploads`). For objects too big for one request (parts arrive
through a proxy with a body cap), the object is never held in memory: parts are
spooled to `$S3_DATA_DIR/.uploads/<uploadId>/<partNumber>` and concatenated to
the blob by streaming, hashing as it goes. Uploads idle for 24 h are swept (at
start, then hourly). Each part is up to 100 MB; 1..10000 parts.

The completed object's **ETag is S3's multipart ETag**, `md5(md5(part1) ||
md5(part2) || ...)` in hex plus `-N`, exactly what AWS returns; a single PUT
gets the plain md5. ETags named in the `CompleteMultipartUpload` body are
checked (`InvalidPart` on mismatch), part numbers must be ascending and exist
(`InvalidPartOrder` / `InvalidPart`), an empty body means "all parts". An upload
belongs to the (owner, bucket, key) that started it; anyone else gets
`NoSuchUpload` 404. Errors: 413 `EntityTooLarge` (part over 100 MB, or total over
`S3_MAX_OBJECT_BYTES`), 409 `OperationAborted` (another request is
completing/aborting the same upload).

## What is supported

ListBuckets; CreateBucket / HeadBucket / DeleteBucket (a non-empty bucket is
refused with `BucketNotEmpty`, like S3) / GetBucketLocation; PutObject;
GetObject (single `Range`, `If-Match` / `If-None-Match` / `If-Modified-Since` /
`If-Unmodified-Since`, 206 / 304 / 412 / 416, `response-*` overrides);
HeadObject; DeleteObject; DeleteObjects; CopyObject (`x-amz-copy-source`,
`x-amz-metadata-directive`); ListObjects v1 and ListObjectsV2 (`prefix`,
`delimiter` with `CommonPrefixes`, `max-keys`, `continuation-token`,
`start-after`, `marker`, `encoding-type=url`); user metadata (`x-amz-meta-*`) and
`Content-Type` / `Cache-Control` / `Content-Disposition` / `Content-Encoding` /
`Content-Language` / `Expires` stored and returned; multipart (above). Keys may
contain `/`, spaces, unicode, `+` and `%`. Listings are in byte order.

Body handling: `x-amz-content-sha256` may be a hex SHA-256 (verified,
`XAmzContentSHA256Mismatch`), `UNSIGNED-PAYLOAD`, or any of the `aws-chunked`
streaming modes (`STREAMING-AWS4-HMAC-SHA256-PAYLOAD`, `...-PAYLOAD-TRAILER`,
`STREAMING-UNSIGNED-PAYLOAD-TRAILER`); chunk signatures and the trailer
signature are verified and `x-amz-decoded-content-length` is honoured. The
streaming body is decoded incrementally from the request body; a large one goes
to a temporary file under `$S3_DATA_DIR/.body-tmp`, never into memory.
`Content-MD5` and `x-amz-checksum-crc32|crc32c|crc64nvme|sha1|sha256` (header or
trailer) are verified (`BadDigest` / `InvalidDigest`); a supplied checksum is
echoed on PutObject / UploadPart and returned by GetObject / HeadObject when
`x-amz-checksum-mode: ENABLED` is sent. `Expect: 100-continue` is handled.

### Not supported

Virtual-hosted-style addressing; SigV4a and the legacy SigV2 / `AWS key:secret`
schemes; versioning, ACLs, bucket policies, lifecycle, tagging, CORS, website,
encryption / SSE, object lock, replication, notifications, `SelectObjectContent`,
POST-policy browser uploads, multi-range `Range` (the whole object is sent),
`If-*` conditions on copy sources, conditional writes (`If-None-Match` on PUT).
Requests using these subresources get 501 `NotImplemented`. Minimum part size
(5 MiB) is not enforced. Composite `x-amz-checksum-*` values on
`CompleteMultipartUpload` are accepted but not verified. Only one
region (`S3_REGION`) exists. There is no `Owner` in object listings.

## Running the tests

Unit tests are plain executables with asserts; the image build runs them. They
include the official AWS SigV4 test vectors (GET / PUT / list / lifecycle / presigned
examples from the S3 documentation, and the documented aws-chunked signature
chain), the `aws-chunked` decoder, checksums and secret encryption:

    cmake -B out -DCMAKE_BUILD_TYPE=Release
    cmake --build out --target s3tests && ./out/s3tests

Integration tests drive a built image against a throwaway postgres (SigV4 auth
via curl, cross-owner isolation, multipart, size cap, secret encryption); the
SDK tests run real clients (boto3, aws-cli v2, `@aws-sdk/client-s3` v3 +
lib-storage, rclone) from containers, against the store directly *and* through
an nginx TLS reverse proxy. CI runs both after the image build:

    docker build -t s3server:test -f server/backend/Dockerfile server
    IMAGE=s3server:test server/backend/tests/integration/run.sh
    IMAGE=s3server:test server/backend/tests/sdk/run.sh [boto3|awscli|node|rclone ...]

The admin UI (`server/frontend`) builds on its own:
`npm ci && npm run lint && npm run typecheck && npm test && npm run build`,
or `docker build server/frontend`. The image proxies `/api/s3/*` to
`S3_BACKEND_URL` (default `http://backend:9000`), read at runtime. The browser
keeps the access key and secret in memory only and sends them to its own
origin in `x-s3-*` request headers; the Next proxy route signs the upstream
request with SigV4 (`UNSIGNED-PAYLOAD`, streamed) and drops those headers. The
secret is never put in a URL, cookie, web storage or log.

## Two rules worth keeping

**Handlers must not run on drogon's IO loops.** Every store call here is
synchronous (`execSqlSync`, filesystem reads and writes). A loop thread
blocked in one of them stops serving *every* connection the kernel hands it
afterwards, so the server answers some requests in milliseconds and silently
never answers others -- clients just hang until their own timeout, and
`/health` looks intermittently dead too. Handlers hand their work to
`Workers` (see `services/Workers.h`, `services/OffLoop.h`) and reply from
there, which drogon allows from any thread. The AuthFilter does the same
(key lookup, signature check, body decoding all run on a worker).

**Blobs are content-addressed, so a file can have more than one owner.**
`bucket/<md5>` means two keys with identical bytes share one file. Deleting a
key must therefore check `ObjectStore::pathInUse()` before unlinking, or the
surviving key answers 200 with an empty body. (CopyObject inside a bucket just
adds another row for the same blob; across buckets it hard-links the file.)

## Default credential

`migrations/002_seed_data.sql` seeds `minioadmin/minioadmin` with `admin`
permissions, but only while the `api_keys` table is empty: the entrypoint
re-runs every migration on each start, and an unconditional insert used to
resurrect that key after a deployment had deleted it. Replace it with a real
key on anything reachable by others.

---
Split out of `metabuilder/services/object-store` as part of the [reposplit](https://github.com/johndoe6345789/reposplit) effort.
