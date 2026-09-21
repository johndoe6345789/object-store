#!/usr/bin/env bash
# Real S3 clients against a freshly built store image, from containers, on a
# private network with a throwaway postgres. Nothing outside the containers,
# network and temp dir created here is touched.
#
#   docker build -t s3server:test -f server/backend/Dockerfile server
#   IMAGE=s3server:test server/backend/tests/sdk/run.sh [suite ...]
#
# suites: boto3 awscli node rclone (default: all). Two endpoints are offered
# to every client, both path-style and region us-east-1:
#   S3_ENDPOINT      http://srv:9000        the store directly
#   S3_TLS_ENDPOINT  https://s3.test        nginx TLS reverse proxy that keeps
#                                           the Host header (what CapRover does)
# Clients trust the proxy's throwaway CA via AWS_CA_BUNDLE=/ca/ca.pem.
set -u
IMAGE=${IMAGE:-s3server:test}
PGIMG=${PGIMG:-postgres:15-alpine}
NGINX=${NGINX_IMAGE:-nginx:alpine}
HERE=$(cd "$(dirname "$0")" && pwd)
ID=s3sdk-$$
WORK=$(mktemp -d)
SUITES=("$@"); [ ${#SUITES[@]} = 0 ] && SUITES=(boto3 awscli node rclone)
trap 'docker rm -f $ID-srv $ID-pg $ID-tls >/dev/null 2>&1; docker network rm $ID >/dev/null 2>&1; docker run --rm -v "$WORK":/w alpine rm -rf /w/* >/dev/null 2>&1; rm -rf "$WORK"' EXIT

rnd() { head -c "$1" /dev/urandom | od -An -tx1 | tr -d ' \n'; }

docker network create $ID >/dev/null || exit 1
docker run -d --name $ID-pg --network $ID --network-alias pg \
  -e POSTGRES_USER=s3 -e POSTGRES_PASSWORD=it -e POSTGRES_DB=s3 "$PGIMG" >/dev/null || exit 1
docker run -d --name $ID-srv --network $ID --network-alias srv \
  -e PGHOST=pg -e PGUSER=s3 -e PGPASSWORD=it -e PGDATABASE=s3 \
  -e S3_DB_CONN="host=pg port=5432 dbname=s3 user=s3 password=it" \
  ${S3_SERVER_ENV:-} "$IMAGE" >/dev/null || exit 1

# --- TLS reverse proxy with a throwaway CA-signed certificate ------------
mkdir -p "$WORK/ca" "$WORK/nginx"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=s3.test" \
  -addext "subjectAltName=DNS:s3.test" -keyout "$WORK/nginx/key.pem" \
  -out "$WORK/ca/ca.pem" >/dev/null 2>&1 || { echo "openssl failed"; exit 1; }
cp "$WORK/ca/ca.pem" "$WORK/nginx/cert.pem"
cat > "$WORK/nginx/default.conf" <<'NGX'
server {
  listen 443 ssl;
  server_name s3.test;
  ssl_certificate /etc/nginx/cert.pem;
  ssl_certificate_key /etc/nginx/key.pem;
  client_max_body_size 0;
  location / {
    proxy_pass http://srv:9000;
    proxy_http_version 1.1;
    proxy_set_header Host $http_host;
    proxy_set_header X-Forwarded-Proto https;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_read_timeout 600s;
  }
}
NGX
chmod -R a+rX "$WORK"
docker run -d --name $ID-tls --network $ID --network-alias s3.test \
  -v "$WORK/nginx/default.conf":/etc/nginx/conf.d/default.conf:ro \
  -v "$WORK/nginx/cert.pem":/etc/nginx/cert.pem:ro \
  -v "$WORK/nginx/key.pem":/etc/nginx/key.pem:ro "$NGINX" >/dev/null || exit 1

for _ in $(seq 90); do
  [ "$(docker inspect -f '{{.State.Running}}' $ID-srv 2>/dev/null)" = true ] &&
    docker exec $ID-srv curl -sf http://127.0.0.1:9000/health >/dev/null 2>&1 && break
  sleep 1
done
docker exec $ID-srv curl -sf http://127.0.0.1:9000/health >/dev/null || {
  echo "server did not start"; docker logs $ID-srv | tail -20; exit 1; }

# --- keys: two owners, one read-only key. Secrets are random, never printed ---
ADMIN_SECRET=$(rnd 20); RO_SECRET=$(rnd 20); OTHER_SECRET=$(rnd 20)
docker exec -i $ID-pg psql -U s3 -d s3 -q >/dev/null <<SQL
INSERT INTO api_keys (access_key, secret_key, owner, permissions) VALUES
 ('sdk-rw',    '$ADMIN_SECRET', 'sdk1', 'read,write'),
 ('sdk-ro',    '$RO_SECRET',    'sdk1', 'read'),
 ('sdk-other', '$OTHER_SECRET', 'sdk2', 'read,write');
SQL

COMMON=(--network $ID
  -v "$HERE":/tests:ro -v "$WORK/ca":/ca:ro -v "$HERE/../../../frontend/src/utils":/fe:ro
  -e S3_ENDPOINT=http://srv:9000 -e S3_TLS_ENDPOINT=https://s3.test
  -e S3_ACCESS_KEY=sdk-rw -e S3_SECRET_KEY=$ADMIN_SECRET
  -e S3_RO_ACCESS_KEY=sdk-ro -e S3_RO_SECRET_KEY=$RO_SECRET
  -e S3_OTHER_ACCESS_KEY=sdk-other -e S3_OTHER_SECRET_KEY=$OTHER_SECRET
  -e S3_REGION=us-east-1 -e AWS_CA_BUNDLE=/ca/ca.pem -e S3_CA_BUNDLE=/ca/ca.pem
  -e SSL_CERT_FILE=/ca/ca.pem -e REQUESTS_CA_BUNDLE=/ca/ca.pem)

RESULTS=()
FAILED=0
run_suite() { # run_suite <name> <docker args...>
  local name=$1; shift
  echo; echo "================ $name ================"
  local log="$WORK/$name.log"
  docker run --rm "${COMMON[@]}" "$@" 2>&1 | tee "$log"
  local rc=${PIPESTATUS[0]}
  local pass fail
  pass=$(grep -c '^  PASS' "$log"); fail=$(grep -c '^  FAIL' "$log")
  [ "$rc" != 0 ] && [ "$fail" = 0 ] && fail=1
  RESULTS+=("$name: $pass passed, $fail failed")
  [ "$rc" != 0 ] || [ "$fail" != 0 ] && FAILED=1
}

for s in "${SUITES[@]}"; do
  case $s in
    boto3)  run_suite boto3 python:3.12-slim \
              sh -c 'pip install -q --no-cache-dir 'boto3[crt]' requests 2>&1 | tail -1; python /tests/boto3/test_boto3.py' ;;
    awscli) run_suite awscli --entrypoint bash amazon/aws-cli /tests/awscli/test_awscli.sh ;;
    node)   run_suite node -w /work node:22 bash /tests/node/run.sh ;;
    rclone) run_suite rclone --entrypoint sh rclone/rclone:latest /tests/rclone/test_rclone.sh ;;
    *) echo "unknown suite $s"; FAILED=1 ;;
  esac
done

echo; echo "================ summary ================"
printf '%s\n' "${RESULTS[@]}"
if [ $FAILED = 0 ]; then echo "all SDK suites passed"; else
  echo "SDK suites FAILED"; docker logs $ID-srv 2>&1 | tail -15; exit 1; fi
