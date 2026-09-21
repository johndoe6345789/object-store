#!/bin/sh
# rclone (env-configured, no config file) against both endpoints. POSIX sh / busybox.
. /tests/lib.sh
export RCLONE_CONFIG_S3_TYPE=s3 RCLONE_CONFIG_S3_PROVIDER=Other \
  RCLONE_CONFIG_S3_ACCESS_KEY_ID="$S3_ACCESS_KEY" RCLONE_CONFIG_S3_SECRET_ACCESS_KEY="$S3_SECRET_KEY" \
  RCLONE_CONFIG_S3_REGION=us-east-1 RCLONE_CONFIG_S3_FORCE_PATH_STYLE=true \
  RCLONE_CONFIG=/nonexistent SSL_CERT_FILE=/ca/ca.pem
rclone version | head -3
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
head -c 60000000 /dev/urandom > "$W/big.bin"
printf 'hello rclone\n' > "$W/small.txt"
MPU="--s3-chunk-size 10M --s3-upload-cutoff 20M"
sum() { md5sum "$1" | cut -d' ' -f1; }

run() {
  tag=$1; ep=$2
  export RCLONE_CONFIG_S3_ENDPOINT=$ep
  B=rcl-$tag-$$-$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n')
  R="s3:$B"
  rm -rf "$W/tree"
  mkdir -p "$W/tree/sub dir/ü ñ" "$W/tree/日本語"
  echo one > "$W/tree/a b.txt"; echo two > "$W/tree/sub dir/ü ñ/çø.txt"; echo three > "$W/tree/日本語/ファイル+1.txt"; echo four > "$W/tree/pct%20.txt"
  t() { d="$tag $1"; shift; if "$@" >"$W/out.log" 2>&1; then pass "$d"; else fail "$d" "$(tail -n2 "$W/out.log" | tr '\n' ' ' | cut -c1-250)"; fi; }
  t "mkdir bucket" rclone mkdir "$R"
  t "lsd lists bucket" sh -c "rclone lsd s3: | grep -q ' $B\$'"
  t "copy small file" rclone copy "$W/small.txt" "$R/"
  t "copy 60 MiB multipart" rclone copy $MPU -v "$W/big.bin" "$R/"
  rm -rf "$W/back"; mkdir "$W/back"
  t "copy back 60 MiB" rclone copy "$R/big.bin" "$W/back/"
  t "md5 of 60 MiB matches after round trip" test "$(sum "$W/back/big.bin")" = "$(sum "$W/big.bin")"
  t "rclone check (size, hash where comparable)" rclone check "$W" "$R" --filter "- back/**" --filter "+ big.bin" --filter "+ small.txt" --filter "- *"
  t "rclone check --download (byte compare)" rclone check --download "$W" "$R" --filter "- back/**" --filter "+ big.bin" --filter "+ small.txt" --filter "- *"
  t "rclone cat small" sh -c "test \"\$(rclone cat $R/small.txt)\" = 'hello rclone'"
  t "rclone ls" sh -c "rclone ls $R | grep -q '60000000 big.bin' && rclone ls $R | grep -q '13 small.txt'"
  t "rclone lsjson fields" sh -c "rclone lsjson $R > $W/ls.json && grep -q '\"ModTime\":\"20' $W/ls.json && grep -q '\"Size\":13' $W/ls.json && grep -q '\"MimeType\":\"[a-z]' $W/ls.json"
  t "rclone size" sh -c "rclone size $R | grep -q 'Total objects: 2'"
  t "sync tree with spaces/unicode" rclone sync "$W/tree" "$R/tree"
  t "sync is idempotent (no transfers)" sh -c "rclone sync -v '$W/tree' '$R/tree' 2>&1 | grep -qv 'Copied'"
  t "check synced tree" rclone check "$W/tree" "$R/tree" --size-only
  t "lsf shows unicode/space keys" sh -c "rclone lsf -R $R/tree > $W/lsf.txt; cat $W/lsf.txt >&2; grep -q '^a b.txt\$' $W/lsf.txt && grep -q 'ü ñ/çø.txt' $W/lsf.txt && grep -q '日本語/ファイル+1.txt' $W/lsf.txt && grep -q 'pct%20.txt' $W/lsf.txt"
  rm -rf "$W/tree2"
  t "sync back and compare tree" sh -c "rclone sync $R/tree '$W/tree2' && diff -r '$W/tree' '$W/tree2'"
  t "lsd with subdirs" sh -c "rclone lsd $R/tree | grep -q 'sub dir'"
  echo changed > "$W/tree/a b.txt"
  t "sync propagates modification and deletion" sh -c "rm '$W/tree/pct%20.txt'; rclone sync '$W/tree' '$R/tree' && test \"\$(rclone cat '$R/tree/a b.txt')\" = changed && ! rclone lsf $R/tree | grep -q 'pct%20'"
  t "server-side copy (copyto)" sh -c "rclone copyto $R/small.txt $R/copy/small2.txt && test \"\$(rclone cat $R/copy/small2.txt)\" = 'hello rclone'"
  t "server-side move (moveto)" sh -c "rclone moveto $R/copy/small2.txt '$R/copy/mv ü.txt' && rclone lsf $R/copy | grep -q 'mv ü.txt' && ! rclone lsf $R/copy | grep -q small2"
  t "copy with 60 MiB --s3-no-check-bucket / checksum on" rclone copyto $MPU --checksum "$W/big.bin" "$R/big2.bin"
  t "rclone delete (files under tree)" rclone delete "$R/tree"
  t "delete removed the files" sh -c "test -z \"\$(rclone lsf -R $R/tree)\""
  t "rclone deletefile" rclone deletefile "$R/small.txt"
  t "purge bucket contents (purge)" rclone purge "$R"
  t "bucket gone after purge" sh -c "! rclone lsd s3: | grep -q ' $B\$'"
  t "rmdir on missing bucket fails cleanly" sh -c "! rclone rmdir $R"
  # mkdir/rmdir round trip on a fresh empty bucket
  t "mkdir again" rclone mkdir "$R"
  t "rmdir empty bucket" rclone rmdir "$R"
}
run http "$S3_ENDPOINT"
run https "$S3_TLS_ENDPOINT"
[ "$FAILS" = 0 ]
