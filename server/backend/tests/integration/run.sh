#!/usr/bin/env bash
# End-to-end check of SigV4 auth, cross-owner isolation and multipart upload
# against a real server + postgres, both in throwaway containers on a private
# network. Requests are signed by curl (--aws-sigv4, curl >= 7.75).
#
#   docker build -t s3server:test -f backend/Dockerfile server   (from repo root)
#   IMAGE=s3server:test server/backend/tests/integration/run.sh
#
# Touches nothing but the containers/network it creates (removed on exit).
set -u
IMAGE=${IMAGE:-s3server:test}
PGIMG=${PGIMG:-postgres:15-alpine}
ID=s3it-$$
WORK=$(mktemp -d)
FAILS=0
trap 'docker rm -f $ID-srv $ID-pg >/dev/null 2>&1; docker network rm $ID >/dev/null 2>&1; rm -rf "$WORK"' EXIT

docker network create $ID >/dev/null || exit 1
docker run -d --name $ID-pg --network $ID --network-alias pg \
  -e POSTGRES_USER=s3 -e POSTGRES_PASSWORD=it -e POSTGRES_DB=s3 "$PGIMG" >/dev/null || exit 1
docker run -d --name $ID-srv --network $ID -p 127.0.0.1::9000 \
  -e PGHOST=pg -e PGUSER=s3 -e PGPASSWORD=it -e PGDATABASE=s3 \
  -e S3_DB_CONN="host=pg port=5432 dbname=s3 user=s3 password=it" \
  -e S3_MAX_OBJECT_BYTES=$((20 * 1024 * 1024)) "$IMAGE" >/dev/null || exit 1
PORT=$(docker port $ID-srv 9000/tcp | head -1 | sed 's/.*://')
URL=http://127.0.0.1:$PORT

for _ in $(seq 60); do curl -sf "$URL/health" >/dev/null 2>&1 && break; sleep 1; done
curl -sf "$URL/health" >/dev/null || { echo "server did not start"; docker logs $ID-srv | tail -20; exit 1; }

psql() { docker exec -i $ID-pg psql -U s3 -d s3 -q "$@"; }
psql <<'SQL'
INSERT INTO api_keys (access_key, secret_key, owner, permissions) VALUES
 ('alice',  's-alice',  'alice', 'read,write'),
 ('aro',    's-aro',    'alice', 'read'),
 ('bob',    's-bob',    'bob',   'read, write'),
 ('sneaky', 's-sneaky', 'bob',   'readonly,overwrite');
SQL

# req <access:secret|-> METHOD PATH [curl args...]  -> sets CODE, body in $WORK/out
# Every request is SigV4-signed by curl itself (path-style, us-east-1).
SIGV4="aws:amz:us-east-1:s3"
req() {
  local cred=$1 m=$2 p=$3; shift 3
  local auth=(); [ "$cred" != "-" ] && auth=(--aws-sigv4 "$SIGV4" --user "$cred")
  if [ "$m" = HEAD ]; then
    CODE=$(curl -s -o /dev/null -I -D "$WORK/hdr" -w '%{http_code}' "${auth[@]}" "$URL$p")
  else
    CODE=$(curl -s -o "$WORK/out" -D "$WORK/hdr" -w '%{http_code}' -X "$m" "${auth[@]}" "$@" "$URL$p")
  fi
}
ok() { # ok <desc> <expected code>
  if [ "$CODE" = "$2" ]; then echo "  ok   $1"; else echo "  FAIL $1 (want $2, got $CODE)"; FAILS=$((FAILS+1)); fi
}
yes() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; FAILS=$((FAILS+1)); fi; }
A=alice:s-alice; ARO=aro:s-aro; B=bob:s-bob; SNEAKY=sneaky:s-sneaky
uid() { grep -o '<UploadId>[0-9a-f]*' "$WORK/out" | sed 's/<UploadId>//'; }
partdirs() { docker exec $ID-srv sh -c 'ls /data/s3/.uploads 2>/dev/null | wc -l'; }
blobs() { docker exec $ID-srv sh -c "ls /data/s3/blobs/$1 2>/dev/null | wc -l"; }

echo "auth (SigV4 only)"
req - GET /health; ok "health is open" 200
req - GET /; ok "no credentials" 403
yes "anonymous is AccessDenied" 'grep -q "<Code>AccessDenied</Code>" "$WORK/out"'
req x:y GET /; ok "unknown key" 403; cp "$WORK/out" "$WORK/unknown"
yes "unknown key is InvalidAccessKeyId" 'grep -q "<Code>InvalidAccessKeyId</Code>" "$WORK/out"'
req alice:wrong GET /; ok "wrong secret" 403
yes "wrong secret is SignatureDoesNotMatch" 'grep -q "<Code>SignatureDoesNotMatch</Code>" "$WORK/out"'
yes "no secret echoed in errors" '! grep -q "s-alice\|wrong" "$WORK/out"'
yes "errors are S3 XML with a RequestId" 'grep -q "<Error><Code>.*</Code><Message>.*</Message>.*<RequestId>[0-9A-F]\{16\}</RequestId>" "$WORK/out"'
yes "x-amz-request-id on every response" 'grep -qi "^x-amz-request-id: [0-9A-F]\{16\}" "$WORK/hdr"'
req "$SNEAKY" GET /; ok "'readonly' token does not grant read" 403
req "$ARO" PUT /ab-ro; ok "read-only key cannot create bucket" 403
CODE=$(curl -s -o "$WORK/out" -w '%{http_code}' -H "Authorization: AWS alice:s-alice" "$URL/")
ok "legacy 'AWS access:secret' header is refused" 403
yes "legacy header: AuthorizationHeaderMalformed" 'grep -q "AuthorizationHeaderMalformed" "$WORK/out"'
CODE=$(curl -s -o "$WORK/out" -w '%{http_code}' -H "Authorization: Bearer abc" "$URL/"); ok "other schemes refused" 403
CODE=$(curl -s -o "$WORK/out" -w '%{http_code}' --aws-sigv4 "aws:amz:eu-west-2:s3" --user alice:s-alice "$URL/")
ok "wrong signing region" 400
yes "wrong region: AuthorizationHeaderMalformed" 'grep -q "AuthorizationHeaderMalformed" "$WORK/out"'
CODE=$(curl -s -o "$WORK/out" -w '%{http_code}' --aws-sigv4 "aws:amz:us-east-1:s3" --user alice:s-alice -H "x-amz-date: 20200101T000000Z" "$URL/")
ok "skewed clock" 403
yes "skewed clock: RequestTimeTooSkewed" 'grep -q "RequestTimeTooSkewed" "$WORK/out"'

echo "buckets and cross-owner isolation"
req $A PUT /ab; ok "alice creates ab" 200
req $A PUT "/a..x"; ok "invalid bucket name" 400
req $A PUT "/a%2F..%2Fb"; yes "traversal-ish name rejected" '[ "$CODE" != 200 ]'
req $B PUT /ab; ok "bob cannot take alice's bucket name" 409
req $B HEAD /ab; ok "bob HEAD alice bucket" 404
req $B GET /ab; ok "bob list alice bucket" 404
req $B GET /; yes "bob's bucket list hides ab" '! grep -q "<Name>ab</Name>" "$WORK/out"'
req $B DELETE /ab; ok "bob cannot delete alice bucket" 404
head -c 300000 /dev/urandom > "$WORK/o1"
req $A PUT /ab/k1 --data-binary @"$WORK/o1"; ok "alice puts k1" 200
req $B GET /ab/k1; ok "bob GET alice object" 404
req $B HEAD /ab/k1; ok "bob HEAD alice object" 404
req $B PUT /ab/k1 --data-binary "hijack"; ok "bob overwrite alice object" 404
req $B DELETE /ab/k1; ok "bob delete alice object" 404
req $A GET /ab/k1; ok "alice object still there" 200
yes "alice object intact" 'cmp -s "$WORK/out" "$WORK/o1"'
req $ARO GET /ab/k1; ok "read-only key of same owner reads" 200
req $ARO DELETE /ab/k1; ok "read-only key cannot delete" 403
req $B PUT /bb; ok "bob creates bb" 200
req $B PUT /bb/k1 --data-binary @"$WORK/o1"; ok "bob stores same bytes" 200
req $B DELETE /bb/k1; ok "bob deletes his copy" 204
req $A GET /ab/k1; ok "alice copy unaffected by bob delete" 200
yes "alice bytes intact" 'cmp -s "$WORK/out" "$WORK/o1"'
req $A PUT /ab/k2 --data-binary @"$WORK/o1"
req $A DELETE /ab/k1; req $A GET /ab/k2; ok "same-bucket dedup: survivor still served" 200
yes "survivor bytes intact" 'cmp -s "$WORK/out" "$WORK/o1"'
req $A PUT /ab/k2 --data-binary "different"; yes "overwrite drops the old blob" '[ "$(blobs ab)" = 1 ]'
req $A PUT "/ab/pre%25x" --data-binary 1; req $A PUT /ab/preZZ --data-binary 1
req $A GET "/ab?prefix=pre%25"; yes "LIKE wildcard in prefix is literal" '! grep -q "preZZ" "$WORK/out"'
req $A PUT /ab/dir/sub/n.txt --data-binary nested; ok "PUT key containing slashes" 200
req $A GET /ab/dir/sub/n.txt; ok "GET key containing slashes" 200
yes "nested key body" '[ "$(cat "$WORK/out")" = nested ]'
req $A HEAD /ab/dir/sub/n.txt; ok "HEAD key containing slashes" 200
req $B GET /ab/dir/sub/n.txt; ok "other owner, nested key" 404
req $A DELETE /ab/dir/sub/n.txt; ok "DELETE key containing slashes" 204
req $A PUT /ab/empty --data-binary ""; req $A GET /ab/empty; ok "empty object GET" 200

echo "multipart"
head -c 5000000 /dev/urandom > "$WORK/p1"; head -c 3000000 /dev/urandom > "$WORK/p2"
cat "$WORK/p1" "$WORK/p2" > "$WORK/whole"
req $A POST "/ab/big.bin?uploads" -H "Content-Type: application/x-test"; ok "initiate" 200
UP=$(uid); yes "upload id is 64 hex chars" '[[ "$UP" =~ ^[0-9a-f]{64}$ ]]'
req $A PUT "/ab/big.bin?partNumber=1&uploadId=$UP" --data-binary "old"; ok "part 1" 200
req $A PUT "/ab/big.bin?partNumber=1&uploadId=$UP" --data-binary @"$WORK/p1"; ok "part 1 retry overwrites" 200
yes "part etag is md5" 'grep -qi "etag: \"$(md5sum < "$WORK/p1" | cut -d" " -f1)\"" "$WORK/hdr"'
req $A PUT "/ab/big.bin?partNumber=2&uploadId=$UP" --data-binary @"$WORK/p2"; ok "part 2" 200
yes "parts on disk" 'docker exec $ID-srv sh -c "test -f /data/s3/.uploads/$UP/1 && test -f /data/s3/.uploads/$UP/2"'
req $A PUT "/ab/big.bin?partNumber=0&uploadId=$UP" --data-binary x; ok "part 0 rejected" 400
req $A PUT "/ab/big.bin?partNumber=10001&uploadId=$UP" --data-binary x; ok "part 10001 rejected" 400
req $A PUT "/ab/big.bin?partNumber=1&uploadId=../../etc" --data-binary x; ok "traversal upload id" 404
req $A PUT "/ab/big.bin?partNumber=1&uploadId=${UP:0:63}z" --data-binary x; ok "non-hex upload id" 404
req $A PUT "/ab/big.bin?partNumber=1&uploadId=$(printf 'f%.0s' $(seq 64))" --data-binary x; ok "unknown upload id" 404
req $B PUT "/bb/big.bin?partNumber=1&uploadId=$UP" --data-binary x; ok "other owner, own bucket" 404
req $B PUT "/ab/big.bin?partNumber=1&uploadId=$UP" --data-binary x; ok "other owner, victim bucket" 404
req $B POST "/bb/big.bin?uploadId=$UP"; ok "other owner cannot complete" 404
req $B DELETE "/bb/big.bin?uploadId=$UP"; ok "other owner cannot abort" 404
req $A PUT "/ab/other.bin?partNumber=1&uploadId=$UP" --data-binary x; ok "key mismatch" 404
req $ARO PUT "/ab/big.bin?partNumber=1&uploadId=$UP" --data-binary x; ok "read-only key cannot upload part" 403
req $A POST "/ab/big.bin?uploadId=$UP" -d '<CompleteMultipartUpload><Part><PartNumber>2</PartNumber></Part><Part><PartNumber>1</PartNumber></Part></CompleteMultipartUpload>'; ok "out-of-order parts" 400
req $A POST "/ab/big.bin?uploadId=$UP" -d '<CompleteMultipartUpload><Part><PartNumber>9</PartNumber></Part></CompleteMultipartUpload>'; ok "missing part" 400
req $A POST "/ab/big.bin?uploadId=$UP" -d '<CompleteMultipartUpload><Part><PartNumber>1</PartNumber></Part><Part><PartNumber>2</PartNumber></Part></CompleteMultipartUpload>'; ok "complete" 200
WANT=$(md5sum < "$WORK/whole" | cut -d' ' -f1)
MPETAG=$(python3 - "$WORK/p1" "$WORK/p2" <<'PY'
import hashlib, sys
d = b"".join(hashlib.md5(open(f, "rb").read()).digest() for f in sys.argv[1:])
print(hashlib.md5(d).hexdigest() + "-2")
PY
)
yes "complete returns S3's md5-of-md5s-N ETag" 'grep -q "$MPETAG" "$WORK/out" && grep -qi "etag: \"$MPETAG\"" "$WORK/hdr"'
yes "part dir removed" '[ "$(partdirs)" = 0 ]'
req $A GET /ab/big.bin; ok "GET assembled" 200
yes "GET bytes equal the parts concatenated" 'cmp -s "$WORK/out" "$WORK/whole"'
yes "content type kept from initiate" 'grep -qi "content-type: application/x-test" "$WORK/hdr"'
req $A HEAD /ab/big.bin; ok "HEAD assembled" 200
req $A GET /ab; yes "listing shows the multipart ETag" 'grep -q "&quot;$MPETAG&quot;" "$WORK/out"'
req $A POST "/ab/big.bin?uploadId=$UP"; ok "completed id is gone" 404
req $B GET /ab/big.bin; ok "bob cannot read assembled object" 404
req $A POST "/ab/nest/ed/m.bin?uploads"; ok "initiate, key with slashes" 200; UPN=$(uid)
req $A PUT "/ab/nest/ed/m.bin?partNumber=1&uploadId=$UPN" --data-binary @"$WORK/p2"; ok "part, key with slashes" 200
req $A POST "/ab/nest/ed/m.bin?uploadId=$UPN"; ok "complete, key with slashes" 200
req $A GET /ab/nest/ed/m.bin; yes "assembled nested key served" 'cmp -s "$WORK/out" "$WORK/p2"'
req $A POST "/ab/copy.bin?uploads"; UP2=$(uid)
req $A PUT "/ab/copy.bin?partNumber=1&uploadId=$UP2" --data-binary @"$WORK/p1"
req $A PUT "/ab/copy.bin?partNumber=2&uploadId=$UP2" --data-binary @"$WORK/p2"
req $A POST "/ab/copy.bin?uploadId=$UP2"; ok "second identical object, empty body = all parts" 200
req $A DELETE /ab/big.bin; ok "delete one of two identical blobs" 204
req $A GET /ab/copy.bin; ok "sibling still served" 200
yes "sibling bytes intact" 'cmp -s "$WORK/out" "$WORK/whole"'
req $A POST "/ab/x.bin?uploads"; UP3=$(uid)
req $A PUT "/ab/x.bin?partNumber=1&uploadId=$UP3" --data-binary @"$WORK/p2"
req $A DELETE "/ab/x.bin?uploadId=$UP3"; ok "abort" 204
yes "abort removed the parts" '[ "$(partdirs)" = 0 ]'
req $A PUT "/ab/x.bin?partNumber=1&uploadId=$UP3" --data-binary x; ok "aborted id is gone" 404
req $A POST "/nobucket/x.bin?uploads"; ok "initiate in missing bucket" 404
req $B POST "/ab/x.bin?uploads"; ok "initiate in other owner's bucket" 404
req $A POST "/ab/x.bin"; ok "bare POST" 501

echo "size cap (S3_MAX_OBJECT_BYTES=20 MiB in this run)"
head -c $((21 * 1024 * 1024)) /dev/zero > "$WORK/toobig"
req $A PUT /ab/toobig --data-binary @"$WORK/toobig"; ok "single PUT over cap" 413
req $A POST "/ab/cap.bin?uploads"; UP4=$(uid)
for n in 1 2; do req $A PUT "/ab/cap.bin?partNumber=$n&uploadId=$UP4" --data-binary @"$WORK/toobig"; done
ok "parts summing past the cap" 413

echo "bucket lifecycle"
req $A DELETE /ab; ok "a bucket that still holds objects is not deleted" 409
yes "BucketNotEmpty" 'grep -q "<Code>BucketNotEmpty</Code>" "$WORK/out"'
req $A GET /ab
OBJS=$(grep -o '<Key>[^<]*</Key>' "$WORK/out" | sed 's|.*|<Object>&</Object>|' | tr -d '\n')
req $A POST "/ab?delete" -d "<Delete>$OBJS</Delete>"; ok "DeleteObjects empties it" 200
yes "DeleteObjects reports the keys" 'grep -q "<Deleted><Key>k2</Key></Deleted>" "$WORK/out"'
req $A GET /ab; yes "bucket is empty" '! grep -q "<Key>" "$WORK/out"'
req $A DELETE /ab; ok "alice deletes bucket" 204
yes "its blobs are gone" '[ "$(blobs ab)" = 0 ]'
req $B PUT /ab; ok "bob can now take the name" 200
req $B GET /ab/k2; ok "no data carried over to the new owner" 404
req $B GET /ab; yes "new bucket is empty" '! grep -q "<Key>" "$WORK/out"'

echo "secret encryption at rest (S3_SECRET_ENCRYPTION_KEY)"
KEY=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
start_srv() { # start_srv <name> [extra docker args...]
  local n=$1; shift
  docker run -d --name $ID-$n --network $ID -p 127.0.0.1::9000 \
    -e PGHOST=pg -e PGUSER=s3 -e PGPASSWORD=it -e PGDATABASE=s3 \
    -e S3_DB_CONN="host=pg port=5432 dbname=s3 user=s3 password=it" "$@" "$IMAGE" >/dev/null || return 1
  URL="http://127.0.0.1:$(docker port $ID-$n 9000/tcp | head -1 | sed 's/.*://')"
  for _ in $(seq 60); do curl -sf "$URL/health" >/dev/null 2>&1 && return 0; sleep 1; done
  return 1
}
URL1=$URL
yes "server starts with a master key" 'start_srv srv2 -e S3_SECRET_ENCRYPTION_KEY=$KEY'
yes "start log says so" 'docker logs $ID-srv2 2>&1 | grep -q "encrypted at rest"'
req $A GET /; ok "encrypted key still authenticates (first use encrypts it)" 200
yes "alice's row is encrypted" '[ "$(psql -tA -c "select count(*) from api_keys where access_key=\$\$alice\$\$ and secret_key=\$\$!enc\$\$ and secret_enc is not null")" = 1 ]'
yes "no plaintext copy of the secret remains" '[ "$(psql -tA -c "select count(*) from api_keys where secret_key like \$\$s-alice%\$\$ or secret_enc like \$\$%s-alice%\$\$")" = 0 ]'
req alice:wrong GET /; ok "wrong secret still refused" 403
req $A GET /; ok "second request uses the ciphertext" 200
psql -c "UPDATE api_keys SET secret_key='s-alice-rotated' WHERE access_key='alice'"
req alice:s-alice-rotated GET /; ok "rotation: a new plaintext secret wins" 200
req $A GET /; ok "rotation: the old secret stops working" 403
yes "rotated secret is encrypted again" '[ "$(psql -tA -c "select secret_key from api_keys where access_key=\$\$alice\$\$")" = "!enc" ]'
psql -c "UPDATE api_keys SET secret_key='s-alice' WHERE access_key='alice'"
docker rm -f $ID-srv2 >/dev/null
yes "server without the key starts (plaintext mode) with a warning" 'start_srv srv3'
yes "warning is logged" 'docker logs $ID-srv3 2>&1 | grep -q "WARNING: S3_SECRET_ENCRYPTION_KEY is not set"'
req $A GET /; ok "plaintext row works without a master key" 200
psql -c "UPDATE api_keys SET secret_key='!enc', secret_enc='AAAA' WHERE access_key='alice'"
req $A GET /; ok "encrypted row without a master key fails closed" 500
docker rm -f $ID-srv3 >/dev/null
start_srv srv4 -e S3_SECRET_ENCRYPTION_KEY=$KEY >/dev/null
req $A GET /; ok "undecryptable blob fails closed" 500
psql -c "UPDATE api_keys SET secret_key='s-alice', secret_enc=NULL WHERE access_key='alice'"
req $A GET /; ok "restored" 200
docker rm -f $ID-srv4 >/dev/null
URL=$URL1

echo
if [ $FAILS = 0 ]; then echo "all integration checks passed"; else echo "$FAILS check(s) FAILED"; docker logs $ID-srv 2>&1 | tail -20; exit 1; fi
