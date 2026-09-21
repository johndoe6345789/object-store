"""boto3 / botocore against the store, unchanged: path-style, us-east-1.

Every scenario runs against the plain-http endpoint and against the TLS
reverse proxy (which changes how SDKs sign and checksum the payload).
Prints `  PASS name` / `  FAIL name (reason)`; exits 1 on any failure.
"""
import base64
import datetime as dt
import hashlib
import io
import os
import sys
import time
import types
import uuid

import boto3
import botocore
import botocore.auth
import requests
from boto3.s3.transfer import TransferConfig
from botocore.client import Config
from botocore.exceptions import ClientError

E = os.environ
FAILS = 0


def report(name, ok, why=""):
    global FAILS
    if ok:
        print(f"  PASS {name}", flush=True)
    else:
        FAILS += 1
        print(f"  FAIL {name} ({why})", flush=True)


def scenario(label):
    def deco(fn):
        try:
            fn()
            report(f"{label}: {fn.__name__}", True)
        except Exception as e:  # noqa: BLE001
            report(f"{label}: {fn.__name__}", False, f"{type(e).__name__}: {str(e)[:300]}")
        return fn
    return deco


def make_client(endpoint, access, secret, **cfg):
    return boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name="us-east-1",
        aws_access_key_id=access,
        aws_secret_access_key=secret,
        config=Config(s3={"addressing_style": "path"}, signature_version="s3v4",
                      retries={"max_attempts": 1}, **cfg),
        verify=E["S3_CA_BUNDLE"] if endpoint.startswith("https") else None,
    )


def err_code(fn):
    try:
        fn()
    except ClientError as e:
        return e.response["Error"]["Code"], e.response["ResponseMetadata"]["HTTPStatusCode"]
    return None, None


def sha(b):
    return hashlib.sha256(b).hexdigest()


def rnd_file(path, mb):
    h = hashlib.sha256()
    with open(path, "wb") as f:
        for _ in range(mb):
            chunk = os.urandom(1024 * 1024)
            f.write(chunk)
            h.update(chunk)
    return h.hexdigest()


def file_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def suite(label, endpoint, big):
    s3 = make_client(endpoint, E["S3_ACCESS_KEY"], E["S3_SECRET_KEY"])
    bad = make_client(endpoint, E["S3_ACCESS_KEY"], "definitely-wrong-secret")
    ro = make_client(endpoint, E["S3_RO_ACCESS_KEY"], E["S3_RO_SECRET_KEY"])
    other = make_client(endpoint, E["S3_OTHER_ACCESS_KEY"], E["S3_OTHER_SECRET_KEY"])
    B = "bt-" + uuid.uuid4().hex[:10]

    @scenario(label)
    def create_head_location_list_buckets():
        s3.create_bucket(Bucket=B)
        s3.head_bucket(Bucket=B)
        assert s3.get_bucket_location(Bucket=B)["LocationConstraint"] in (None, "", "us-east-1")
        names = [b["Name"] for b in s3.list_buckets()["Buckets"]]
        assert B in names
        assert isinstance(s3.list_buckets()["Buckets"][0]["CreationDate"], dt.datetime)

    @scenario(label)
    def put_get_head_delete():
        r = s3.put_object(Bucket=B, Key="a.txt", Body=b"hello world")
        assert r["ETag"] == '"5eb63bbbe01eeed093cb22bb8f5acdc3"', r["ETag"]
        assert s3.get_object(Bucket=B, Key="a.txt")["Body"].read() == b"hello world"
        h = s3.head_object(Bucket=B, Key="a.txt")
        assert h["ContentLength"] == 11 and h["ETag"] == r["ETag"]
        assert h["LastModified"].year >= 2024
        s3.delete_object(Bucket=B, Key="a.txt")
        assert err_code(lambda: s3.get_object(Bucket=B, Key="a.txt"))[0] == "NoSuchKey"
        assert err_code(lambda: s3.head_object(Bucket=B, Key="a.txt"))[1] == 404
        s3.delete_object(Bucket=B, Key="a.txt")  # idempotent

    @scenario(label)
    def empty_and_binary_objects():
        s3.put_object(Bucket=B, Key="empty", Body=b"")
        assert s3.get_object(Bucket=B, Key="empty")["Body"].read() == b""
        blob = os.urandom(1_500_000)
        s3.put_object(Bucket=B, Key="blob", Body=blob)
        assert s3.get_object(Bucket=B, Key="blob")["Body"].read() == blob

    @scenario(label)
    def metadata_and_headers_round_trip():
        s3.put_object(Bucket=B, Key="m", Body=b"x", ContentType="text/plain",
                      CacheControl="max-age=60", ContentDisposition="attachment; filename=x.txt",
                      ContentEncoding="identity", ContentLanguage="en",
                      Metadata={"Color": "blue", "owner-id": "42"})
        for r in (s3.head_object(Bucket=B, Key="m"), s3.get_object(Bucket=B, Key="m")):
            assert r["ContentType"] == "text/plain", r["ContentType"]
            assert r["CacheControl"] == "max-age=60"
            assert r["ContentDisposition"] == "attachment; filename=x.txt"
            assert r["Metadata"] == {"color": "blue", "owner-id": "42"}, r["Metadata"]

    @scenario(label)
    def weird_keys_round_trip():
        keys = ["with space.txt", "üñí/çödé/файл.txt", "plus+sign", "percent%20literal",
                "a/b/c/", "semi;colon,comma", "quote'\"paren()", "tilde~star*bang!", "dot/../dot",
                "trailing space ", "=eq&amp?q#hash", "日本語/キー", "emoji-😀"]
        for k in keys:
            s3.put_object(Bucket=B, Key=f"w/{k}", Body=k.encode())
        for k in keys:
            assert s3.get_object(Bucket=B, Key=f"w/{k}")["Body"].read() == k.encode(), k
            assert s3.head_object(Bucket=B, Key=f"w/{k}")["ContentLength"] == len(k.encode())
        listed = [o["Key"] for p in s3.get_paginator("list_objects_v2").paginate(Bucket=B, Prefix="w/")
                  for o in p.get("Contents", [])]
        assert sorted(listed) == sorted(f"w/{k}" for k in keys), listed
        for k in keys:
            s3.delete_object(Bucket=B, Key=f"w/{k}")
        assert "Contents" not in s3.list_objects_v2(Bucket=B, Prefix="w/")

    @scenario(label)
    def list_v2_prefix_delimiter_pagination():
        for i in range(25):
            s3.put_object(Bucket=B, Key=f"l/p{i:02d}/f", Body=b"1")
        for i in range(7):
            s3.put_object(Bucket=B, Key=f"l/top{i}", Body=b"1")
        r = s3.list_objects_v2(Bucket=B, Prefix="l/", Delimiter="/")
        assert len(r["CommonPrefixes"]) == 25 and len(r["Contents"]) == 7, (len(r["CommonPrefixes"]), len(r["Contents"]))
        assert r["CommonPrefixes"][0]["Prefix"] == "l/p00/" and r["KeyCount"] == 32
        assert not r["IsTruncated"]
        # pages of 10 across prefixes and objects, no repeats, nothing lost
        seen, tok, pages = [], None, 0
        while True:
            kw = dict(Bucket=B, Prefix="l/", Delimiter="/", MaxKeys=10)
            if tok:
                kw["ContinuationToken"] = tok
            r = s3.list_objects_v2(**kw)
            pages += 1
            seen += [p["Prefix"] for p in r.get("CommonPrefixes", [])]
            seen += [o["Key"] for o in r.get("Contents", [])]
            assert r["KeyCount"] <= 10
            if not r["IsTruncated"]:
                break
            tok = r["NextContinuationToken"]
        assert pages == 4 and len(seen) == 32 and len(set(seen)) == 32, (pages, len(seen))
        assert seen == sorted(seen)
        flat = [o["Key"] for p in s3.get_paginator("list_objects_v2").paginate(Bucket=B, Prefix="l/", PaginationConfig={"PageSize": 9})
                for o in p.get("Contents", [])]
        assert len(flat) == 32
        r = s3.list_objects_v2(Bucket=B, Prefix="l/", StartAfter="l/p23/f", MaxKeys=1000)
        assert [o["Key"] for o in r["Contents"]][:1] == ["l/p24/f"]
        r = s3.list_objects_v2(Bucket=B, MaxKeys=0)
        assert r["KeyCount"] == 0 and not r["IsTruncated"]
        r = s3.list_objects(Bucket=B, Prefix="l/", Delimiter="/", MaxKeys=5)  # v1
        assert r["IsTruncated"] and len(r["CommonPrefixes"]) == 5
        r2 = s3.list_objects(Bucket=B, Prefix="l/", Delimiter="/", Marker=r["NextMarker"], MaxKeys=1000)
        assert len(r2["CommonPrefixes"]) == 20 and len(r2["Contents"]) == 7
        for i in range(25):
            s3.delete_object(Bucket=B, Key=f"l/p{i:02d}/f")
        for i in range(7):
            s3.delete_object(Bucket=B, Key=f"l/top{i}")

    @scenario(label)
    def ranged_and_conditional_gets():
        s3.put_object(Bucket=B, Key="r", Body=b"0123456789")
        r = s3.get_object(Bucket=B, Key="r", Range="bytes=2-5")
        assert r["Body"].read() == b"2345" and r["ContentRange"] == "bytes 2-5/10"
        assert r["ResponseMetadata"]["HTTPStatusCode"] == 206
        assert s3.get_object(Bucket=B, Key="r", Range="bytes=-3")["Body"].read() == b"789"
        assert s3.get_object(Bucket=B, Key="r", Range="bytes=7-")["Body"].read() == b"789"
        assert err_code(lambda: s3.get_object(Bucket=B, Key="r", Range="bytes=50-60"))[1] == 416
        etag = s3.head_object(Bucket=B, Key="r")["ETag"]
        assert err_code(lambda: s3.get_object(Bucket=B, Key="r", IfNoneMatch=etag))[1] == 304
        assert s3.get_object(Bucket=B, Key="r", IfMatch=etag)["Body"].read() == b"0123456789"
        assert err_code(lambda: s3.get_object(Bucket=B, Key="r", IfMatch='"nope"'))[1] == 412
        past = dt.datetime(2001, 1, 1, tzinfo=dt.timezone.utc)
        assert err_code(lambda: s3.get_object(Bucket=B, Key="r", IfUnmodifiedSince=past))[1] == 412
        future = dt.datetime.now(dt.timezone.utc) + dt.timedelta(days=1)
        assert err_code(lambda: s3.get_object(Bucket=B, Key="r", IfModifiedSince=future))[1] == 304

    @scenario(label)
    def checksums_and_content_md5():
        for algo in ("CRC32", "CRC32C", "SHA1", "SHA256"):
            r = s3.put_object(Bucket=B, Key="ck", Body=b"checksummed", ChecksumAlgorithm=algo)
            assert r["ETag"], algo
            assert r.get(f"Checksum{algo}"), f"{algo} not echoed"
        md5 = base64.b64encode(hashlib.md5(b"md5body").digest()).decode()
        s3.put_object(Bucket=B, Key="md5", Body=b"md5body", ContentMD5=md5)
        wrong = base64.b64encode(hashlib.md5(b"other").digest()).decode()
        assert err_code(lambda: s3.put_object(Bucket=B, Key="md5b", Body=b"md5body", ContentMD5=wrong))[0] == "BadDigest"
        assert err_code(lambda: s3.head_object(Bucket=B, Key="md5b"))[1] == 404  # nothing stored
        g = s3.get_object(Bucket=B, Key="ck", ChecksumMode="ENABLED")
        assert g["Body"].read() == b"checksummed"

    @scenario(label)
    def copy_object():
        s3.put_object(Bucket=B, Key="src key+1.txt", Body=b"copy me", ContentType="text/x-src",
                      Metadata={"a": "1"})
        s3.copy_object(Bucket=B, Key="dst/copy é.txt", CopySource={"Bucket": B, "Key": "src key+1.txt"})
        g = s3.get_object(Bucket=B, Key="dst/copy é.txt")
        assert g["Body"].read() == b"copy me" and g["ContentType"] == "text/x-src" and g["Metadata"] == {"a": "1"}
        s3.copy_object(Bucket=B, Key="dst/replaced", CopySource=f"{B}/src key+1.txt",
                       MetadataDirective="REPLACE", Metadata={"b": "2"}, ContentType="text/y")
        g = s3.get_object(Bucket=B, Key="dst/replaced")
        assert g["Metadata"] == {"b": "2"} and g["ContentType"] == "text/y"
        s3.copy_object(Bucket=B, Key="src key+1.txt", CopySource={"Bucket": B, "Key": "src key+1.txt"},
                       MetadataDirective="REPLACE", Metadata={"self": "1"})
        assert s3.head_object(Bucket=B, Key="src key+1.txt")["Metadata"] == {"self": "1"}
        assert err_code(lambda: s3.copy_object(Bucket=B, Key="x", CopySource={"Bucket": B, "Key": "missing"}))[0] == "NoSuchKey"
        # deleting the source leaves the copy intact (shared bytes are not shared fate)
        s3.delete_object(Bucket=B, Key="src key+1.txt")
        assert s3.get_object(Bucket=B, Key="dst/replaced")["Body"].read() == b"copy me"
        other_b = B + "-2"
        s3.create_bucket(Bucket=other_b)
        s3.copy_object(Bucket=other_b, Key="x", CopySource={"Bucket": B, "Key": "dst/replaced"})
        s3.delete_object(Bucket=B, Key="dst/replaced")
        assert s3.get_object(Bucket=other_b, Key="x")["Body"].read() == b"copy me"
        s3.delete_object(Bucket=other_b, Key="x")
        s3.delete_bucket(Bucket=other_b)

    @scenario(label)
    def delete_objects_bulk():
        keys = [f"del/{i} k+é" for i in range(12)]
        for k in keys:
            s3.put_object(Bucket=B, Key=k, Body=b"1")
        r = s3.delete_objects(Bucket=B, Delete={"Objects": [{"Key": k} for k in keys[:8]]})
        assert sorted(d["Key"] for d in r["Deleted"]) == sorted(keys[:8]), r
        r = s3.delete_objects(Bucket=B, Delete={"Objects": [{"Key": k} for k in keys[8:]], "Quiet": True})
        assert "Deleted" not in r
        assert "Contents" not in s3.list_objects_v2(Bucket=B, Prefix="del/")

    @scenario(label)
    def multipart_small_api_incl_list_parts_uploads_abort_and_part_copy():
        key = "mp/api+test.bin"
        up = s3.create_multipart_upload(Bucket=B, Key=key, ContentType="application/x-mp",
                                        Metadata={"k": "v"})
        uid = up["UploadId"]
        assert any(u["UploadId"] == uid for u in s3.list_multipart_uploads(Bucket=B, Prefix="mp/").get("Uploads", []))
        parts, data = [], b""
        for n in (1, 2, 3):
            chunk = os.urandom(300_000 + n)
            data += chunk
            r = s3.upload_part(Bucket=B, Key=key, UploadId=uid, PartNumber=n, Body=chunk)
            assert r["ETag"] == '"' + hashlib.md5(chunk).hexdigest() + '"'
            parts.append({"PartNumber": n, "ETag": r["ETag"]})
        lp = s3.list_parts(Bucket=B, Key=key, UploadId=uid)
        assert [p["PartNumber"] for p in lp["Parts"]] == [1, 2, 3] and lp["Parts"][0]["Size"] == 300_001
        lp = s3.list_parts(Bucket=B, Key=key, UploadId=uid, MaxParts=2)
        assert lp["IsTruncated"] and lp["NextPartNumberMarker"] == 2
        # wrong etag is refused
        bad = [dict(parts[0], ETag='"' + "0" * 32 + '"')] + parts[1:]
        assert err_code(lambda: s3.complete_multipart_upload(Bucket=B, Key=key, UploadId=uid, MultipartUpload={"Parts": bad}))[0] == "InvalidPart"
        r = s3.complete_multipart_upload(Bucket=B, Key=key, UploadId=uid, MultipartUpload={"Parts": parts})
        want = hashlib.md5(b"".join(hashlib.md5(data[i:j]).digest() for i, j in
                                    ((0, 300_001), (300_001, 600_003), (600_003, len(data))))).hexdigest() + "-3"
        assert r["ETag"] == f'"{want}"', (r["ETag"], want)
        g = s3.get_object(Bucket=B, Key=key)
        assert g["Body"].read() == data and g["ContentType"] == "application/x-mp" and g["Metadata"] == {"k": "v"}
        assert s3.head_object(Bucket=B, Key=key)["ETag"] == f'"{want}"'
        assert err_code(lambda: s3.list_parts(Bucket=B, Key=key, UploadId=uid))[0] == "NoSuchUpload"
        # abort
        up2 = s3.create_multipart_upload(Bucket=B, Key="mp/abort")
        s3.upload_part(Bucket=B, Key="mp/abort", UploadId=up2["UploadId"], PartNumber=1, Body=b"x" * 10)
        s3.abort_multipart_upload(Bucket=B, Key="mp/abort", UploadId=up2["UploadId"])
        assert err_code(lambda: s3.upload_part(Bucket=B, Key="mp/abort", UploadId=up2["UploadId"], PartNumber=1, Body=b"x"))[0] == "NoSuchUpload"
        assert not [u for u in s3.list_multipart_uploads(Bucket=B).get("Uploads", []) if u["Key"] == "mp/abort"]
        # UploadPartCopy of a byte range
        up3 = s3.create_multipart_upload(Bucket=B, Key="mp/copied")
        r1 = s3.upload_part_copy(Bucket=B, Key="mp/copied", UploadId=up3["UploadId"], PartNumber=1,
                                 CopySource={"Bucket": B, "Key": key}, CopySourceRange="bytes=10-99999")
        assert r1["CopyPartResult"]["ETag"] == '"' + hashlib.md5(data[10:100000]).hexdigest() + '"'
        s3.complete_multipart_upload(Bucket=B, Key="mp/copied", UploadId=up3["UploadId"],
                                     MultipartUpload={"Parts": [{"PartNumber": 1, "ETag": r1["CopyPartResult"]["ETag"]}]})
        assert s3.get_object(Bucket=B, Key="mp/copied")["Body"].read() == data[10:100000]

    @scenario(label)
    def transfer_manager_upload_download_file():
        size = 120 if big else 20
        p = f"/tmp/tm-{label}.bin"
        want = rnd_file(p, size)
        cfg = TransferConfig(multipart_threshold=8 * 1024 * 1024, multipart_chunksize=8 * 1024 * 1024)
        s3.upload_file(p, B, "tm/big file+é.bin", Config=cfg,
                       ExtraArgs={"ContentType": "application/x-big", "Metadata": {"n": "1"}})
        h = s3.head_object(Bucket=B, Key="tm/big file+é.bin")
        assert h["ContentLength"] == size * 1024 * 1024 and "-" in h["ETag"], h["ETag"]
        assert h["ContentType"] == "application/x-big" and h["Metadata"] == {"n": "1"}
        out = f"/tmp/tm-{label}.out"
        s3.download_file(B, "tm/big file+é.bin", out, Config=cfg)
        assert file_sha(out) == want
        os.remove(p)
        os.remove(out)

    if big:
        @scenario(label)
        def multipart_120mb_in_5_parts_of_24mb():
            key = "mp/big-120.bin"
            uid = s3.create_multipart_upload(Bucket=B, Key=key)["UploadId"]
            h, parts, md5s = hashlib.sha256(), [], b""
            for n in range(1, 6):
                chunk = os.urandom(24 * 1024 * 1024)
                h.update(chunk)
                md5s += hashlib.md5(chunk).digest()
                r = s3.upload_part(Bucket=B, Key=key, UploadId=uid, PartNumber=n, Body=chunk)
                parts.append({"PartNumber": n, "ETag": r["ETag"]})
            r = s3.complete_multipart_upload(Bucket=B, Key=key, UploadId=uid, MultipartUpload={"Parts": parts})
            assert r["ETag"] == '"' + hashlib.md5(md5s).hexdigest() + '-5"', r["ETag"]
            g = s3.get_object(Bucket=B, Key=key)
            h2 = hashlib.sha256()
            n = 0
            for chunk in g["Body"].iter_chunks(1 << 20):
                h2.update(chunk)
                n += len(chunk)
            assert n == 120 * 1024 * 1024 and h2.hexdigest() == h.hexdigest()
            rg = s3.get_object(Bucket=B, Key=key, Range="bytes=100000000-100000009")
            assert len(rg["Body"].read()) == 10

    @scenario(label)
    def presigned_get_and_put_via_requests():
        s3.put_object(Bucket=B, Key="ps/a b+c.txt", Body=b"presigned body", ContentType="text/plain")
        url = s3.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ps/a b+c.txt"}, ExpiresIn=60)
        assert "X-Amz-Signature=" in url
        r = requests.get(url, verify=E["S3_CA_BUNDLE"] if url.startswith("https") else True)
        assert r.status_code == 200 and r.content == b"presigned body" and r.headers["Content-Type"] == "text/plain"
        url = s3.generate_presigned_url("head_object", Params={"Bucket": B, "Key": "ps/a b+c.txt"}, ExpiresIn=60)
        r = requests.head(url, verify=E["S3_CA_BUNDLE"] if url.startswith("https") else True)
        assert r.status_code == 200 and r.headers["Content-Length"] == "14"
        url = s3.generate_presigned_url("put_object", Params={"Bucket": B, "Key": "ps/uploaded é.bin"}, ExpiresIn=60)
        big = os.urandom(3_000_000)
        r = requests.put(url, data=big, verify=E["S3_CA_BUNDLE"] if url.startswith("https") else True)
        assert r.status_code == 200, r.text
        assert s3.get_object(Bucket=B, Key="ps/uploaded é.bin")["Body"].read() == big
        # presigned URL for another key / method does not work
        url = s3.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ps/a b+c.txt"}, ExpiresIn=60)
        r = requests.get(url.replace("a%20b%2Bc", "uploaded"), verify=E["S3_CA_BUNDLE"] if url.startswith("https") else True)
        assert r.status_code == 403 and "SignatureDoesNotMatch" in r.text
        r = requests.put(url, data=b"x", verify=E["S3_CA_BUNDLE"] if url.startswith("https") else True)
        assert r.status_code == 403
        # ?response-content-disposition etc. are signed query params and pass through
        url = s3.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ps/a b+c.txt",
                                                              "ResponseContentType": "text/csv"}, ExpiresIn=60)
        assert requests.get(url, verify=E["S3_CA_BUNDLE"] if url.startswith("https") else True).status_code == 200

    @scenario(label)
    def presigned_url_expiry_and_max_lifetime():
        url = s3.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ps/a b+c.txt"}, ExpiresIn=2)
        v = E["S3_CA_BUNDLE"] if url.startswith("https") else True
        assert requests.get(url, verify=v).status_code == 200
        time.sleep(3.2)
        r = requests.get(url, verify=v)
        assert r.status_code == 403 and "<Code>AccessDenied</Code>" in r.text and "expired" in r.text, r.text
        url = s3.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ps/a b+c.txt"}, ExpiresIn=604800)
        assert requests.get(url, verify=v).status_code == 200
        url = s3.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ps/a b+c.txt"}, ExpiresIn=604801)
        r = requests.get(url, verify=v)
        assert r.status_code == 400 and "AuthorizationQueryParametersError" in r.text

    @scenario(label)
    def wrong_secret_is_signature_does_not_match():
        c = err_code(lambda: bad.list_buckets())
        assert c == ("SignatureDoesNotMatch", 403), c
        c = err_code(lambda: bad.put_object(Bucket=B, Key="never", Body=b"x"))
        assert c[0] == "SignatureDoesNotMatch"
        unknown = make_client(endpoint, "no-such-key", "whatever")
        assert err_code(lambda: unknown.list_buckets())[0] == "InvalidAccessKeyId"
        assert err_code(lambda: s3.head_object(Bucket=B, Key="never"))[1] == 404

    @scenario(label)
    def clock_skew_is_refused():
        # botocore's pure-python signer (the CRT one ignores the patched clock)
        from botocore.auth import S3SigV4Auth
        from botocore.awsrequest import AWSRequest
        from botocore.credentials import Credentials
        v = E["S3_CA_BUNDLE"] if endpoint.startswith("https") else True

        def signed_list(shift):
            real = botocore.auth.get_current_datetime
            botocore.auth.get_current_datetime = lambda *a, **k: real(*a, **k) + shift
            try:
                r = AWSRequest(method="GET", url=endpoint + "/", headers={})
                S3SigV4Auth(Credentials(E["S3_ACCESS_KEY"], E["S3_SECRET_KEY"]), "s3", "us-east-1").add_auth(r)
            finally:
                botocore.auth.get_current_datetime = real
            return requests.get(r.url, headers=dict(r.headers), verify=v)

        assert signed_list(dt.timedelta(0)).status_code == 200
        for shift in (-dt.timedelta(hours=1), dt.timedelta(hours=1)):
            r = signed_list(shift)
            assert r.status_code == 403 and "<Code>RequestTimeTooSkewed</Code>" in r.text, (shift, r.status_code, r.text[:200])
        assert signed_list(dt.timedelta(minutes=10)).status_code == 200  # within 15 minutes

    @scenario(label)
    def tampered_signed_header_is_refused():
        # x-amz-meta-* is signed as a header; change, drop or add one on the wire
        url = s3.generate_presigned_url("put_object", Params={"Bucket": B, "Key": "tamper", "Metadata": {"a": "1"}}, ExpiresIn=60)
        assert "x-amz-meta-a" in url.lower(), url
        v = E["S3_CA_BUNDLE"] if url.startswith("https") else True
        for headers in ({"x-amz-meta-a": "2"}, {}):
            r = requests.put(url, data=b"x", headers=headers, verify=v)
            assert r.status_code == 403 and "SignatureDoesNotMatch" in r.text, (headers, r.status_code, r.text[:200])
        r = requests.put(url, data=b"x", headers={"x-amz-meta-a": "1", "x-amz-meta-b": "unsigned"}, verify=v)
        assert r.status_code == 403 and "not signed" in r.text, r.text[:200]  # unsigned x-amz-meta-*
        assert requests.put(url, data=b"x", headers={"x-amz-meta-a": "1"}, verify=v).status_code == 200
        assert s3.head_object(Bucket=B, Key="tamper")["Metadata"] == {"a": "1"}

    @scenario(label)
    def read_only_key_reads_but_cannot_write():
        s3.put_object(Bucket=B, Key="ro.txt", Body=b"ro")
        assert ro.get_object(Bucket=B, Key="ro.txt")["Body"].read() == b"ro"
        assert ro.list_objects_v2(Bucket=B)["KeyCount"] >= 1
        assert err_code(lambda: ro.put_object(Bucket=B, Key="w", Body=b"x")) == ("AccessDenied", 403)
        assert err_code(lambda: ro.delete_object(Bucket=B, Key="ro.txt"))[0] == "AccessDenied"
        assert err_code(lambda: ro.create_bucket(Bucket=B + "-ro"))[0] == "AccessDenied"
        assert err_code(lambda: ro.create_multipart_upload(Bucket=B, Key="mpw"))[0] == "AccessDenied"
        assert err_code(lambda: ro.copy_object(Bucket=B, Key="c", CopySource={"Bucket": B, "Key": "ro.txt"}))[0] == "AccessDenied"
        assert err_code(lambda: ro.delete_objects(Bucket=B, Delete={"Objects": [{"Key": "ro.txt"}]}))[0] == "AccessDenied"
        url = ro.generate_presigned_url("put_object", Params={"Bucket": B, "Key": "w2"}, ExpiresIn=60)
        v = E["S3_CA_BUNDLE"] if url.startswith("https") else True
        assert requests.put(url, data=b"x", verify=v).status_code == 403
        url = ro.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ro.txt"}, ExpiresIn=60)
        assert requests.get(url, verify=v).content == b"ro"

    @scenario(label)
    def other_owner_cannot_touch_the_bucket():
        assert B not in [b["Name"] for b in other.list_buckets()["Buckets"]]
        assert err_code(lambda: other.head_bucket(Bucket=B))[1] == 404
        assert err_code(lambda: other.list_objects_v2(Bucket=B))[0] == "NoSuchBucket"
        assert err_code(lambda: other.get_object(Bucket=B, Key="ro.txt"))[0] in ("NoSuchBucket", "NoSuchKey")
        assert err_code(lambda: other.put_object(Bucket=B, Key="hijack", Body=b"x"))[0] == "NoSuchBucket"
        assert err_code(lambda: other.delete_object(Bucket=B, Key="ro.txt"))[0] == "NoSuchBucket"
        assert err_code(lambda: other.create_bucket(Bucket=B))[0] == "BucketAlreadyExists"
        assert err_code(lambda: other.copy_object(Bucket=B + "-x", Key="k", CopySource={"Bucket": B, "Key": "ro.txt"}))[0] in ("NoSuchBucket",)
        # a presigned URL made by the other owner does not reach into this one
        url = other.generate_presigned_url("get_object", Params={"Bucket": B, "Key": "ro.txt"}, ExpiresIn=60)
        v = E["S3_CA_BUNDLE"] if url.startswith("https") else True
        assert requests.get(url, verify=v).status_code == 404
        assert s3.get_object(Bucket=B, Key="ro.txt")["Body"].read() == b"ro"

    @scenario(label)
    def errors_are_s3_xml_with_request_id():
        r = s3.meta.client if False else None  # noqa: F841
        try:
            s3.get_object(Bucket=B, Key="missing-key")
        except ClientError as e:
            m = e.response["ResponseMetadata"]
            assert e.response["Error"]["Code"] == "NoSuchKey"
            assert m["HTTPHeaders"].get("x-amz-request-id"), m
            assert e.response["Error"].get("Message")
        try:
            s3.head_bucket(Bucket=B + "-nope")
        except ClientError as e:
            assert e.response["ResponseMetadata"]["HTTPStatusCode"] == 404

    @scenario(label)
    def bucket_delete_rules_and_cleanup():
        assert err_code(lambda: s3.delete_bucket(Bucket=B))[0] == "BucketNotEmpty"
        assert err_code(lambda: s3.create_bucket(Bucket=B))[0] == "BucketAlreadyOwnedByYou"
        for p in s3.get_paginator("list_multipart_uploads").paginate(Bucket=B):
            for u in p.get("Uploads", []):
                s3.abort_multipart_upload(Bucket=B, Key=u["Key"], UploadId=u["UploadId"])
        for p in s3.get_paginator("list_objects_v2").paginate(Bucket=B):
            objs = [{"Key": o["Key"]} for o in p.get("Contents", [])]
            if objs:
                s3.delete_objects(Bucket=B, Delete={"Objects": objs})
        s3.delete_bucket(Bucket=B)
        assert err_code(lambda: s3.head_bucket(Bucket=B))[1] == 404


print(f"boto3 {boto3.__version__} / botocore {botocore.__version__}", flush=True)
suite("http", E["S3_ENDPOINT"], big=True)
suite("tls", E["S3_TLS_ENDPOINT"], big=False)
print(f"\n{'FAILED: ' + str(FAILS) if FAILS else 'all boto3 scenarios passed'}")
sys.exit(1 if FAILS else 0)
