#!/usr/bin/env bash
# aws-cli v2 against the store, unchanged: `aws --endpoint-url ... s3 ...`,
# path-style, us-east-1, default flexible-checksum behaviour (aws-chunked
# uploads with a trailing checksum) -- no AWS_REQUEST_CHECKSUM_CALCULATION.
. /tests/lib.sh
export AWS_DEFAULT_REGION=us-east-1 AWS_EC2_METADATA_DISABLED=true
export AWS_PAGER="" AWS_CONFIG_FILE=/tmp/awsconfig
export AWS_ACCESS_KEY_ID=$S3_ACCESS_KEY AWS_SECRET_ACCESS_KEY=$S3_SECRET_KEY
aws configure set default.s3.addressing_style path
aws --version

W=$(mktemp -d)
head -c 1000 /dev/urandom > "$W/small.bin"
head -c 104857600 /dev/urandom > "$W/big.bin"          # 100 MiB -> multipart
BIGSUM=$(sha256sum < "$W/big.bin" | cut -d' ' -f1)
SMALLSUM=$(sha256sum < "$W/small.bin" | cut -d' ' -f1)
sumof() { sha256sum < "$1" | cut -d' ' -f1; }

run() {  # run <label> <endpoint> <ca-bundle-or-empty>
  local L=$1 EP=$2
  local B=cli-$L-$RANDOM$RANDOM
  local A=(aws --endpoint-url "$EP")
  echo "--- $L: $EP"
  local -a NOCA=(); [ "$L" = tls ] && export AWS_CA_BUNDLE=/ca/ca.pem || unset AWS_CA_BUNDLE

  ok()  { pass "$L: $1"; }
  bad() { fail "$L: $1" "$2"; }
  t()   { local d=$1; shift; local out; if out=$("$@" 2>&1); then ok "$d"; else bad "$d" "$(echo "$out" | tail -2 | tr '\n' ' ')"; fi; }

  t "s3 mb" "${A[@]}" s3 mb "s3://$B"
  "${A[@]}" s3 ls | grep -q "$B" && ok "s3 ls (buckets)" || bad "s3 ls (buckets)" "bucket missing"
  "${A[@]}" s3api head-bucket --bucket "$B" >/dev/null 2>&1 && ok "s3api head-bucket" || bad "s3api head-bucket" ""
  [ "$("${A[@]}" s3api get-bucket-location --bucket "$B" --query LocationConstraint --output text 2>&1)" != "" ] && ok "get-bucket-location" || bad "get-bucket-location" ""

  # --- small object (aws-chunked + trailing checksum by default)
  t "cp small file up" "${A[@]}" s3 cp "$W/small.bin" "s3://$B/small.bin"
  t "cp small file down" "${A[@]}" s3 cp "s3://$B/small.bin" "$W/small.$L.down"
  [ "$(sumof "$W/small.$L.down")" = "$SMALLSUM" ] && ok "small file round trip" || bad "small file round trip" "sha mismatch"
  H=$("${A[@]}" s3api head-object --bucket "$B" --key small.bin 2>&1)
  echo "$H" | grep -q '"ContentLength": 1000' && ok "s3api head-object" || bad "s3api head-object" "$H"
  echo "$H" | grep -q '"ETag": "\\"[0-9a-f]\{32\}\\""' && ok "single-part ETag is md5" || bad "single-part ETag is md5" "$H"

  # --- 100 MiB: multipart (8 MiB parts, aws-chunked parts)
  t "cp 100MiB up (multipart)" "${A[@]}" s3 cp "$W/big.bin" "s3://$B/big.bin"
  H=$("${A[@]}" s3api head-object --bucket "$B" --key big.bin 2>&1)
  echo "$H" | grep -q '"ContentLength": 104857600' && ok "100MiB size" || bad "100MiB size" "$H"
  echo "$H" | grep -q '"ETag": "\\"[0-9a-f]\{32\}-13\\""' && ok "multipart ETag md5-of-md5s-13" || bad "multipart ETag md5-of-md5s-13" "$H"
  t "cp 100MiB down (ranged parts)" "${A[@]}" s3 cp "s3://$B/big.bin" "$W/big.$L.down"
  [ "$(sumof "$W/big.$L.down")" = "$BIGSUM" ] && ok "100MiB round trip" || bad "100MiB round trip" "sha mismatch"
  rm -f "$W/big.$L.down"
  [ "$("${A[@]}" s3api list-multipart-uploads --bucket "$B" --query 'length(Uploads || `[]`)' --output text)" = 0 ] && ok "no dangling multipart uploads" || bad "no dangling multipart uploads" ""

  # --- names: spaces, unicode, plus, nesting
  mkdir -p "$W/tree/sub dir/ünï" "$W/tree/plain"
  echo one > "$W/tree/a b+c.txt"; echo two > "$W/tree/sub dir/ünï/файл 日本.txt"; echo three > "$W/tree/plain/x.txt"
  echo four > "$W/tree/plain/y%20z.txt"
  t "sync up" "${A[@]}" s3 sync "$W/tree" "s3://$B/tree/"
  LS=$("${A[@]}" s3 ls "s3://$B/tree/" --recursive 2>&1)
  [ "$(echo "$LS" | grep -c .)" = 4 ] && ok "s3 ls --recursive shows 4 objects" || bad "s3 ls --recursive shows 4 objects" "$LS"
  echo "$LS" | grep -q "sub dir/ünï/файл 日本.txt" && ok "unicode/space key listed" || bad "unicode/space key listed" "$LS"
  "${A[@]}" s3 ls "s3://$B/tree/" | grep -q "PRE plain/" && ok "s3 ls shows PRE (CommonPrefixes)" || bad "s3 ls shows PRE (CommonPrefixes)" ""
  t "sync is a no-op when nothing changed" bash -c "[ -z \"\$('${A[0]}' ${A[1]} ${A[2]} s3 sync '$W/tree' 's3://$B/tree/' 2>&1)\" ]"
  mkdir -p "$W/back"
  t "sync down" "${A[@]}" s3 sync "s3://$B/tree/" "$W/back"
  [ "$(cat "$W/back/sub dir/ünï/файл 日本.txt")" = two ] && [ "$(cat "$W/back/a b+c.txt")" = one ] && [ "$(cat "$W/back/plain/y%20z.txt")" = four ] && ok "synced content matches" || bad "synced content matches" ""
  echo changed > "$W/tree/plain/x.txt"
  "${A[@]}" s3 sync "$W/tree" "s3://$B/tree/" | grep -q "upload: .*x.txt" && ok "sync uploads only the changed file" || bad "sync uploads only the changed file" ""
  t "sync --delete" "${A[@]}" s3 sync "$W/tree" "s3://$B/tree/" --delete

  # --- metadata & content type
  t "cp with --metadata/--content-type/--cache-control" "${A[@]}" s3 cp "$W/small.bin" "s3://$B/meta.bin" --metadata k1=v1,k2=v2 --content-type text/x-cli --cache-control max-age=5
  H=$("${A[@]}" s3api head-object --bucket "$B" --key meta.bin 2>&1)
  echo "$H" | grep -q '"k1": "v1"' && echo "$H" | grep -q '"ContentType": "text/x-cli"' && echo "$H" | grep -q 'max-age=5' && ok "metadata round trip" || bad "metadata round trip" "$H"

  # --- s3api: put with each checksum algorithm, list, delete, copy, mv
  for algo in CRC32 CRC32C SHA1 SHA256 CRC64NVME; do
    "${A[@]}" s3api put-object --bucket "$B" --key "ck/$algo" --body "$W/small.bin" --checksum-algorithm $algo >/dev/null 2>&1 && ok "put-object --checksum-algorithm $algo" || bad "put-object --checksum-algorithm $algo" "$("${A[@]}" s3api put-object --bucket "$B" --key ck/$algo --body "$W/small.bin" --checksum-algorithm $algo 2>&1 | tail -1)"
  done
  J=$("${A[@]}" s3api list-objects-v2 --bucket "$B" --prefix ck/ --delimiter / --max-items 2 2>&1)
  echo "$J" | grep -q NextToken && ok "list-objects-v2 --max-items paginates" || bad "list-objects-v2 --max-items paginates" "$J"
  [ "$("${A[@]}" s3api list-objects-v2 --bucket "$B" --prefix tree/ --delimiter / --query 'CommonPrefixes[].Prefix' --output text)" = "tree/plain/	tree/sub dir/" ] && ok "list-objects-v2 --delimiter" || bad "list-objects-v2 --delimiter" ""
  [ "$("${A[@]}" s3api list-objects --bucket "$B" --prefix ck/ --query 'length(Contents)' --output text)" = 5 ] && ok "list-objects (v1)" || bad "list-objects (v1)" ""
  t "s3 cp server-side copy" "${A[@]}" s3 cp "s3://$B/small.bin" "s3://$B/copy/small copy.bin"
  t "s3 mv" "${A[@]}" s3 mv "s3://$B/copy/small copy.bin" "s3://$B/moved.bin"
  "${A[@]}" s3api head-object --bucket "$B" --key "copy/small copy.bin" >/dev/null 2>&1 && bad "mv removed the source" "" || ok "mv removed the source"
  [ "$("${A[@]}" s3 cp "s3://$B/small.bin" - 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$SMALLSUM" ] && ok "s3 cp to stdout" || bad "s3 cp to stdout" ""
  t "s3 cp from stdin" bash -c "echo streamed | '${A[0]}' ${A[1]} ${A[2]} s3 cp - 's3://$B/stdin.txt'"
  [ "$("${A[@]}" s3 cp "s3://$B/stdin.txt" - 2>/dev/null)" = streamed ] && ok "stdin upload content" || bad "stdin upload content" ""
  t "s3 cp --recursive" "${A[@]}" s3 cp "s3://$B/tree" "$W/rec" --recursive
  t "s3api delete-objects" "${A[@]}" s3api delete-objects --bucket "$B" --delete '{"Objects":[{"Key":"ck/CRC32"},{"Key":"ck/SHA1"}]}'

  # --- presigned URLs, fetched by plain curl (like a browser)
  U=$("${A[@]}" s3 presign "s3://$B/tree/a b+c.txt" --expires-in 60)
  [ "$(curl -s ${AWS_CA_BUNDLE:+--cacert $AWS_CA_BUNDLE} "$U")" = one ] && ok "presigned GET via curl" || bad "presigned GET via curl" "$U"
  [ "$(curl -s -o /dev/null -w '%{http_code}' ${AWS_CA_BUNDLE:+--cacert $AWS_CA_BUNDLE} "${U/a%20b/a%20x}")" = 403 ] && ok "tampered presigned URL refused" || bad "tampered presigned URL refused" ""
  U=$("${A[@]}" s3 presign "s3://$B/tree/a b+c.txt" --expires-in 1); sleep 3
  curl -s ${AWS_CA_BUNDLE:+--cacert $AWS_CA_BUNDLE} "$U" | grep -q "Request has expired" && ok "expired presigned URL refused" || bad "expired presigned URL refused" ""

  # --- authorisation
  AWS_SECRET_ACCESS_KEY=wrong "${A[@]}" s3 ls 2>&1 | grep -q SignatureDoesNotMatch && ok "wrong secret: SignatureDoesNotMatch" || bad "wrong secret: SignatureDoesNotMatch" ""
  AWS_ACCESS_KEY_ID=$S3_RO_ACCESS_KEY AWS_SECRET_ACCESS_KEY=$S3_RO_SECRET_KEY "${A[@]}" s3 ls "s3://$B/" >/dev/null 2>&1 && ok "read-only key lists" || bad "read-only key lists" ""
  AWS_ACCESS_KEY_ID=$S3_RO_ACCESS_KEY AWS_SECRET_ACCESS_KEY=$S3_RO_SECRET_KEY "${A[@]}" s3 cp "$W/small.bin" "s3://$B/ro.bin" 2>&1 | grep -q AccessDenied && ok "read-only key cannot upload (AccessDenied)" || bad "read-only key cannot upload (AccessDenied)" ""
  AWS_ACCESS_KEY_ID=$S3_OTHER_ACCESS_KEY AWS_SECRET_ACCESS_KEY=$S3_OTHER_SECRET_KEY "${A[@]}" s3 ls "s3://$B/" 2>&1 | grep -q NoSuchBucket && ok "other owner: NoSuchBucket" || bad "other owner: NoSuchBucket" ""

  # --- cleanup: rb of a non-empty bucket fails, --force empties and removes it
  "${A[@]}" s3 rb "s3://$B" 2>&1 | grep -q BucketNotEmpty && ok "s3 rb refuses a non-empty bucket" || bad "s3 rb refuses a non-empty bucket" ""
  t "s3 rm --recursive" "${A[@]}" s3 rm "s3://$B" --recursive
  [ -z "$("${A[@]}" s3 ls "s3://$B/" 2>&1)" ] && ok "bucket is empty after rm --recursive" || bad "bucket is empty after rm --recursive" ""
  t "s3 rb" "${A[@]}" s3 rb "s3://$B"
  "${A[@]}" s3 ls | grep -q "$B" && bad "bucket gone from ls" "" || ok "bucket gone from ls"
  rm -rf "$W/back" "$W/rec"
}

run http "$S3_ENDPOINT"
run tls  "$S3_TLS_ENDPOINT"
[ $FAILS = 0 ] && echo "all aws-cli scenarios passed" || echo "FAILED: $FAILS"
[ $FAILS = 0 ]
