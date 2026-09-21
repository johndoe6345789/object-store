#!/usr/bin/env bash
# Runs inside node:22, cwd /work, /tests mounted read-only.
set -eu
export NODE_EXTRA_CA_CERTS=/ca/ca.pem
echo "node $(node --version)"
npm init -y >/dev/null
npm i --no-audit --no-fund --loglevel=error @aws-sdk/client-s3 @aws-sdk/lib-storage @aws-sdk/s3-request-presigner >/dev/null
for p in client-s3 lib-storage s3-request-presigner; do
  echo "@aws-sdk/$p $(node -p "require('/work/node_modules/@aws-sdk/$p/package.json').version")"
done
cp /tests/node/test.mjs ./test.mjs
node --experimental-strip-types --no-warnings /tests/node/frontend_signer.mjs || FE=1
node test.mjs
rc=$?
[ "${FE:-0}" = 0 ] && exit $rc || exit 1
