import {
  S3Client, CreateBucketCommand, HeadBucketCommand, GetBucketLocationCommand, ListBucketsCommand,
  PutObjectCommand, GetObjectCommand, HeadObjectCommand, ListObjectsV2Command, CopyObjectCommand,
  DeleteObjectsCommand, DeleteObjectCommand, DeleteBucketCommand,
} from "@aws-sdk/client-s3";
import { Upload } from "@aws-sdk/lib-storage";
import { getSignedUrl } from "@aws-sdk/s3-request-presigner";
import { Readable } from "node:stream";
import crypto from "node:crypto";

const env = process.env;
const REGION = "us-east-1";
const mk = (ak, sk, endpoint) =>
  new S3Client({ region: REGION, endpoint, forcePathStyle: true, credentials: { accessKeyId: ak, secretAccessKey: sk } });

let failures = 0;
const sha = (b) => crypto.createHash("sha256").update(b).digest("hex");
async function body(res) { return Buffer.from(await res.Body.transformToByteArray()); }
async function streamSha(res) {
  const h = crypto.createHash("sha256"); let n = 0;
  for await (const c of res.Body) { h.update(c); n += c.length; }
  return { hex: h.digest("hex"), n };
}
function eq(a, b, what = "value") { if (a !== b) throw new Error(`${what}: got ${JSON.stringify(a)?.slice(0, 120)} want ${JSON.stringify(b)?.slice(0, 120)}`); }
async function errOf(p) { try { await p; } catch (e) { return e; } throw new Error("expected error, got success"); }

async function scenario(tag, name, fn) {
  try { await fn(); console.log(`  PASS ${tag} ${name}`); }
  catch (e) {
    failures++;
    const m = `${e.name || ""}: ${e.message || e}`.replace(/\s+/g, " ").slice(0, 300);
    console.log(`  FAIL ${tag} ${name} (${m})`);
  }
}

async function runEndpoint(tag, endpoint) {
  const s3 = mk(env.S3_ACCESS_KEY, env.S3_SECRET_KEY, endpoint);
  const bucket = `nodesdk-${tag}-${crypto.randomBytes(5).toString("hex")}`;
  const S = (name, fn) => scenario(tag, name, fn);
  const put = (Key, Body, extra = {}) => s3.send(new PutObjectCommand({ Bucket: bucket, Key, Body, ...extra }));
  const get = (Key, extra = {}) => s3.send(new GetObjectCommand({ Bucket: bucket, Key, ...extra }));
  const created = [];
  const listAll = async (extra = {}) => {
    const out = []; let ContinuationToken;
    do {
      const r = await s3.send(new ListObjectsV2Command({ Bucket: bucket, ContinuationToken, ...extra }));
      out.push(...(r.Contents || []).map((o) => o.Key)); ContinuationToken = r.NextContinuationToken;
    } while (ContinuationToken);
    return out;
  };

  await S("CreateBucket", async () => { await s3.send(new CreateBucketCommand({ Bucket: bucket })); });
  await S("HeadBucket", async () => { await s3.send(new HeadBucketCommand({ Bucket: bucket })); });
  await S("GetBucketLocation", async () => {
    const r = await s3.send(new GetBucketLocationCommand({ Bucket: bucket }));
    if (r.LocationConstraint && r.LocationConstraint !== REGION) throw new Error("location " + r.LocationConstraint);
  });
  await S("ListBuckets", async () => {
    const r = await s3.send(new ListBucketsCommand({}));
    if (!(r.Buckets || []).some((b) => b.Name === bucket)) throw new Error("bucket missing from list");
  });

  await S("PutObject small string (default CRC32 trailer)", async () => {
    await put("small.txt", "hello world");
    eq((await body(await get("small.txt"))).toString(), "hello world");
  });
  const mib = crypto.randomBytes(1 << 20);
  await S("PutObject 1 MiB buffer", async () => {
    await put("mib.bin", mib);
    eq(sha(await body(await get("mib.bin"))), sha(mib));
  });
  await S("PutObject Readable stream with ContentLength", async () => {
    const data = crypto.randomBytes(300000);
    await put("stream.bin", Readable.from([data.subarray(0, 100000), data.subarray(100000)]), { ContentLength: data.length });
    eq(sha(await body(await get("stream.bin"))), sha(data));
  });
  await S("PutObject empty body", async () => {
    await put("empty.txt", "");
    const r = await get("empty.txt"); eq((await body(r)).length, 0, "len");
  });

  await S("GetObject content-type/metadata/cache-control/disposition round trip", async () => {
    await put("meta.txt", "meta", {
      ContentType: "text/plain; charset=utf-8", Metadata: { foo: "bar", "x-two": "2" },
      CacheControl: "max-age=60", ContentDisposition: 'attachment; filename="a.txt"', ContentEncoding: "identity",
    });
    const r = await get("meta.txt");
    eq(r.ContentType, "text/plain; charset=utf-8", "ContentType");
    eq(r.Metadata?.foo, "bar", "Metadata.foo"); eq(r.Metadata?.["x-two"], "2", "Metadata.x-two");
    eq(r.CacheControl, "max-age=60", "CacheControl");
    eq(r.ContentDisposition, 'attachment; filename="a.txt"', "ContentDisposition");
    eq((await body(r)).toString(), "meta");
  });
  await S("HeadObject", async () => {
    const r = await s3.send(new HeadObjectCommand({ Bucket: bucket, Key: "small.txt" }));
    eq(r.ContentLength, 11, "ContentLength");
    if (!r.ETag) throw new Error("no ETag");
    if (!(r.LastModified instanceof Date) || isNaN(r.LastModified)) throw new Error("bad LastModified");
    const e = await errOf(s3.send(new HeadObjectCommand({ Bucket: bucket, Key: "nope" })));
    eq(e.$metadata?.httpStatusCode, 404, "missing head status");
  });
  await S("Ranged GetObject (206, ContentRange)", async () => {
    const r = await get("small.txt", { Range: "bytes=2-5" });
    eq(r.$metadata.httpStatusCode, 206, "status"); eq(r.ContentRange, "bytes 2-5/11", "ContentRange");
    eq((await body(r)).toString(), "llo ");
    const s = await get("small.txt", { Range: "bytes=-3" }); eq((await body(s)).toString(), "rld", "suffix range");
    const o = await get("small.txt", { Range: "bytes=6-" }); eq((await body(o)).toString(), "world", "open range");
  });
  await S("Conditional GET IfNoneMatch -> NotModified", async () => {
    const h = await s3.send(new HeadObjectCommand({ Bucket: bucket, Key: "small.txt" }));
    const e = await errOf(get("small.txt", { IfNoneMatch: h.ETag }));
    // bodyless 304: the SDK cannot derive a Code from the response, so it reports
    // name "NotModified" only on some versions; the status is what is checked.
    eq(e.$metadata?.httpStatusCode, 304, "status");
    if (!["NotModified", "Unknown", "UnknownError"].includes(e.name)) throw new Error("unexpected name " + e.name);
  });
  await S("Conditional GET IfMatch mismatch -> PreconditionFailed", async () => {
    const e = await errOf(get("small.txt", { IfMatch: '"deadbeef"' }));
    eq(e.$metadata?.httpStatusCode, 412, "status");
  });

  for (const alg of ["SHA256", "CRC32C", "CRC32", "SHA1"]) {
    await S(`PutObject ChecksumAlgorithm ${alg}`, async () => {
      const data = crypto.randomBytes(70000);
      await put(`ck-${alg}.bin`, data, { ChecksumAlgorithm: alg });
      eq(sha(await body(await get(`ck-${alg}.bin`))), sha(data));
    });
  }

  await S("lib-storage Upload 60 MiB multipart (part 10 MiB, queue 3)", async () => {
    const big = crypto.randomBytes(60 * 1024 * 1024);
    const up = new Upload({ client: s3, params: { Bucket: bucket, Key: "big.bin", Body: big }, partSize: 10 * 1024 * 1024, queueSize: 3 });
    let parts = 0; up.on("httpUploadProgress", () => parts++);
    await up.done();
    const h = await s3.send(new HeadObjectCommand({ Bucket: bucket, Key: "big.bin" }));
    eq(h.ContentLength, big.length, "size");
    if (!/-6"?$/.test(h.ETag || "")) throw new Error("multipart ETag expected -6, got " + h.ETag);
    const r = await get("big.bin"); const s = await streamSha(r);
    eq(s.n, big.length, "streamed len"); eq(s.hex, sha(big), "sha256");
  });
  await S("lib-storage Upload small (single part path)", async () => {
    await new Upload({ client: s3, params: { Bucket: bucket, Key: "lib-small.txt", Body: Readable.from(["abc", "def"]) } }).done();
    eq((await body(await get("lib-small.txt"))).toString(), "abcdef");
  });

  const odd = ["with space.txt", "üñí/çødé/日本語.txt", "plus+sign.txt", "per%cent%20.txt", "a/b/c/deep.txt", "trailing/", "q?x=1&y=2#frag.txt", "semi;colon,comma=eq@at.txt", "star*paren(x)!'.txt"];
  await S("odd keys: put/get", async () => {
    for (const k of odd) {
      await put(k, `data:${k}`);
      eq((await body(await get(k))).toString(), `data:${k}`, "body of " + k);
    }
  });
  await S("odd keys: list returns exact keys (EncodingType url default)", async () => {
    const all = await listAll();
    for (const k of odd) if (!all.includes(k)) throw new Error("missing in list: " + k);
  });
  await S("odd keys: HeadObject", async () => {
    for (const k of odd) await s3.send(new HeadObjectCommand({ Bucket: bucket, Key: k }));
  });
  await S("odd keys: CopyObject (CopySource URL-encoded)", async () => {
    for (const k of odd) {
      const dst = "copy/" + k;
      await s3.send(new CopyObjectCommand({ Bucket: bucket, Key: dst, CopySource: `${bucket}/${k.split("/").map(encodeURIComponent).join("/")}` }));
      eq((await body(await get(dst))).toString(), `data:${k}`, "copied " + k);
    }
  });
  await S("CopyObject with MetadataDirective REPLACE", async () => {
    const r = await s3.send(new CopyObjectCommand({ Bucket: bucket, Key: "copy-meta.txt", CopySource: `${bucket}/meta.txt`, MetadataDirective: "REPLACE", Metadata: { n: "v" }, ContentType: "text/x-test" }));
    if (!r.CopyObjectResult?.ETag) throw new Error("no CopyObjectResult.ETag");
    const g = await get("copy-meta.txt"); eq(g.Metadata?.n, "v", "meta"); eq(g.ContentType, "text/x-test", "ct");
  });
  await S("odd keys: delete", async () => {
    for (const k of odd) {
      await s3.send(new DeleteObjectCommand({ Bucket: bucket, Key: k }));
      const e = await errOf(get(k)); eq(e.name, "NoSuchKey", "after delete " + k);
    }
    const all = await listAll();
    for (const k of odd) if (all.includes(k)) throw new Error("still listed: " + k);
  });

  await S("ListObjectsV2 Prefix/Delimiter -> CommonPrefixes", async () => {
    for (const k of ["d/a.txt", "d/b.txt", "d/sub/c.txt", "d/sub/d.txt", "e/x.txt"]) await put(k, "x");
    const r = await s3.send(new ListObjectsV2Command({ Bucket: bucket, Prefix: "d/", Delimiter: "/" }));
    eq((r.Contents || []).map((o) => o.Key).sort().join(","), "d/a.txt,d/b.txt", "Contents");
    eq((r.CommonPrefixes || []).map((p) => p.Prefix).join(","), "d/sub/", "CommonPrefixes");
    const t = await s3.send(new ListObjectsV2Command({ Bucket: bucket, Delimiter: "/" }));
    const cps = (t.CommonPrefixes || []).map((p) => p.Prefix);
    for (const p of ["d/", "e/"]) if (!cps.includes(p)) throw new Error("top-level prefix missing " + p);
  });
  await S("ListObjectsV2 MaxKeys pagination (25 keys, pages of 10)", async () => {
    const keys = Array.from({ length: 25 }, (_, i) => `page/k${String(i).padStart(2, "0")}`);
    for (let i = 0; i < keys.length; i += 5) await Promise.all(keys.slice(i, i + 5).map((k) => put(k, "p")));
    const seen = []; let tok, pages = 0;
    do {
      const r = await s3.send(new ListObjectsV2Command({ Bucket: bucket, Prefix: "page/", MaxKeys: 10, ContinuationToken: tok }));
      pages++;
      if (r.KeyCount > 10 || (r.Contents || []).length > 10) throw new Error("page over MaxKeys");
      if (pages < 3 && !r.IsTruncated) throw new Error("page " + pages + " not truncated");
      seen.push(...r.Contents.map((o) => o.Key)); tok = r.IsTruncated ? r.NextContinuationToken : undefined;
      if (r.IsTruncated && !tok) throw new Error("truncated without token");
    } while (tok && pages < 10);
    eq(pages, 3, "pages"); eq(seen.join(","), keys.join(","), "keys in order");
  });
  await S("ListObjectsV2 StartAfter", async () => {
    const r = await s3.send(new ListObjectsV2Command({ Bucket: bucket, Prefix: "page/", StartAfter: "page/k22" }));
    eq((r.Contents || []).map((o) => o.Key).join(","), "page/k23,page/k24");
  });

  await S("DeleteObjects (Quiet false) returns Deleted list", async () => {
    const ks = ["dm/1", "dm/2 x", "dm/3+é"]; for (const k of ks) await put(k, "z");
    const r = await s3.send(new DeleteObjectsCommand({ Bucket: bucket, Delete: { Objects: ks.map((Key) => ({ Key })), Quiet: false } }));
    eq((r.Deleted || []).map((d) => d.Key).sort().join("|"), [...ks].sort().join("|"), "Deleted");
    eq((r.Errors || []).length, 0, "Errors");
    for (const k of ks) eq((await errOf(get(k))).name, "NoSuchKey", "gone " + k);
  });
  await S("DeleteObjects Quiet true", async () => {
    await put("dq/1", "z");
    const r = await s3.send(new DeleteObjectsCommand({ Bucket: bucket, Delete: { Objects: [{ Key: "dq/1" }], Quiet: true } }));
    eq((r.Errors || []).length, 0, "Errors");
    eq((await errOf(get("dq/1"))).name, "NoSuchKey");
  });
  await S("DeleteObject (and idempotent on missing key)", async () => {
    await put("del.txt", "x");
    const r = await s3.send(new DeleteObjectCommand({ Bucket: bucket, Key: "del.txt" }));
    eq(r.$metadata.httpStatusCode, 204, "status");
    await s3.send(new DeleteObjectCommand({ Bucket: bucket, Key: "del.txt" }));
  });

  await S("NoSuchKey error name", async () => {
    const e = await errOf(get("does-not-exist")); eq(e.name, "NoSuchKey"); eq(e.$metadata.httpStatusCode, 404, "status");
  });
  await S("NoSuchBucket error name", async () => {
    const e = await errOf(s3.send(new GetObjectCommand({ Bucket: bucket + "-nope", Key: "x" })));
    eq(e.name, "NoSuchBucket");
    const l = await errOf(s3.send(new ListObjectsV2Command({ Bucket: bucket + "-nope" }))); eq(l.name, "NoSuchBucket", "list");
  });
  await S("HeadBucket on missing bucket -> 404", async () => {
    const e = await errOf(s3.send(new HeadBucketCommand({ Bucket: bucket + "-nope" }))); eq(e.$metadata.httpStatusCode, 404);
  });

  await S("presigned GET fetched with fetch", async () => {
    const url = await getSignedUrl(s3, new GetObjectCommand({ Bucket: bucket, Key: "small.txt" }), { expiresIn: 300 });
    const r = await fetch(url); eq(r.status, 200, "status"); eq(await r.text(), "hello world");
  });
  await S("presigned GET with odd key and response overrides", async () => {
    await put("pre signed+é.txt", "odd");
    const url = await getSignedUrl(s3, new GetObjectCommand({ Bucket: bucket, Key: "pre signed+é.txt" }), { expiresIn: 300 });
    const r = await fetch(url); eq(r.status, 200, "status"); eq(await r.text(), "odd");
  });
  await S("presigned GET honours response-content-type/-disposition overrides", async () => {
    const url = await getSignedUrl(s3, new GetObjectCommand({ Bucket: bucket, Key: "small.txt", ResponseContentType: "text/x-over", ResponseContentDisposition: "attachment" }), { expiresIn: 300 });
    const r = await fetch(url); eq(r.status, 200, "status"); await r.text();
    eq(r.headers.get("content-type"), "text/x-over", "response-content-type");
    eq(r.headers.get("content-disposition"), "attachment", "response-content-disposition");
  });
  await S("presigned PUT fetched with fetch", async () => {
    const url = await getSignedUrl(s3, new PutObjectCommand({ Bucket: bucket, Key: "presigned-put.txt", ContentType: "text/plain" }), { expiresIn: 300 });
    const r = await fetch(url, { method: "PUT", body: "put via presign", headers: { "content-type": "text/plain" } });
    eq(r.status, 200, "status");
    eq((await body(await get("presigned-put.txt"))).toString(), "put via presign");
  });
  await S("presigned GET with tampered signature -> 403", async () => {
    const url = await getSignedUrl(s3, new GetObjectCommand({ Bucket: bucket, Key: "small.txt" }), { expiresIn: 300 });
    const r = await fetch(url.replace(/X-Amz-Signature=([0-9a-f]{4})/, (m, a) => "X-Amz-Signature=" + (a === "0000" ? "1111" : "0000")));
    eq(r.status, 403, "status"); await r.text();
  });
  await S("presigned GET expired -> 403", async () => {
    const url = await getSignedUrl(s3, new GetObjectCommand({ Bucket: bucket, Key: "small.txt" }), { expiresIn: 1 });
    await new Promise((r) => setTimeout(r, 3000));
    const r = await fetch(url); eq(r.status, 403, "status"); await r.text();
  });

  await S("wrong secret -> SignatureDoesNotMatch", async () => {
    const bad = mk(env.S3_ACCESS_KEY, "x".repeat(40), endpoint);
    const e = await errOf(bad.send(new ListBucketsCommand({}))); eq(e.name, "SignatureDoesNotMatch");
    const e2 = await errOf(bad.send(new PutObjectCommand({ Bucket: bucket, Key: "bad", Body: "x" }))); eq(e2.name, "SignatureDoesNotMatch", "put");
  });
  await S("unknown access key -> InvalidAccessKeyId", async () => {
    const bad = mk("no-such-key", "y".repeat(40), endpoint);
    const e = await errOf(bad.send(new ListBucketsCommand({}))); eq(e.name, "InvalidAccessKeyId");
  });
  await S("read-only key: GET ok, PUT -> AccessDenied", async () => {
    const ro = mk(env.S3_RO_ACCESS_KEY, env.S3_RO_SECRET_KEY, endpoint);
    const r = await ro.send(new GetObjectCommand({ Bucket: bucket, Key: "small.txt" }));
    eq((await body(r)).toString(), "hello world");
    const e = await errOf(ro.send(new PutObjectCommand({ Bucket: bucket, Key: "ro.txt", Body: "x" }))); eq(e.name, "AccessDenied", "put");
    const d = await errOf(ro.send(new DeleteObjectCommand({ Bucket: bucket, Key: "small.txt" }))); eq(d.name, "AccessDenied", "delete");
  });
  await S("cross-owner key cannot see the bucket", async () => {
    const o = mk(env.S3_OTHER_ACCESS_KEY, env.S3_OTHER_SECRET_KEY, endpoint);
    const e = await errOf(o.send(new GetObjectCommand({ Bucket: bucket, Key: "small.txt" })));
    if (e.name !== "NoSuchBucket" && e.$metadata?.httpStatusCode !== 404) throw new Error("got " + e.name);
    const l = await o.send(new ListBucketsCommand({}));
    if ((l.Buckets || []).some((b) => b.Name === bucket)) throw new Error("bucket visible in other owner's ListBuckets");
    const w = await errOf(o.send(new PutObjectCommand({ Bucket: bucket, Key: "evil", Body: "x" })));
    if (w.name !== "NoSuchBucket" && w.$metadata?.httpStatusCode !== 404 && w.name !== "AccessDenied") throw new Error("put got " + w.name);
  });

  // cleanup
  await scenario(tag, "cleanup (delete all objects, DeleteBucket)", async () => {
    for (let k = await listAll(); k.length; k = await listAll()) {
      await s3.send(new DeleteObjectsCommand({ Bucket: bucket, Delete: { Objects: k.slice(0, 1000).map((Key) => ({ Key })), Quiet: true } }));
    }
    await s3.send(new DeleteBucketCommand({ Bucket: bucket }));
    const e = await errOf(s3.send(new HeadBucketCommand({ Bucket: bucket }))); eq(e.$metadata.httpStatusCode, 404, "head after delete");
  });
}

await runEndpoint("http", env.S3_ENDPOINT);
await runEndpoint("https", env.S3_TLS_ENDPOINT);
console.log(failures ? `\n${failures} failure(s)` : "\nall node scenarios passed");
process.exit(failures ? 1 : 0);
