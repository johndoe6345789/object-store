// Interop: the admin UI's own SigV4 signer (server/frontend/src/utils/sigv4.ts,
// mounted at /fe) against the real store, exactly as the Next proxy uses it:
// UNSIGNED-PAYLOAD, streamed body, encoded path/query.
import { signV4, canonicalUri, canonicalQuery } from '/fe/sigv4.ts';

const EP = process.env.S3_ENDPOINT;
const host = new URL(EP).host;
let fails = 0;
const ok = (n, c, why = '') => { console.log(`  ${c ? 'PASS' : 'FAIL'} ui-signer: ${n}${c ? '' : ` (${why})`}`); if (!c) fails++; };

async function call(method, path, { query = '', body, key = process.env.S3_ACCESS_KEY, secret = process.env.S3_SECRET_KEY } = {}) {
  const s = signV4({ method, path, query, host, accessKey: key, secretKey: secret, region: 'us-east-1' });
  const headers = { 'x-amz-date': s.amzDate, 'x-amz-content-sha256': s.payloadHash, authorization: s.authorization };
  const q = canonicalQuery(query);
  return fetch(`${EP}${canonicalUri(path)}${q ? `?${q}` : ''}`, { method, headers, body });
}

const B = 'ui-' + Math.random().toString(36).slice(2, 10);
const key = 'dir/a b+c é 日本.txt';
let r = await call('PUT', `/${B}`); ok('create bucket', r.status === 200, r.status);
r = await call('PUT', `/${B}/${key}`, { body: 'ui body' }); ok('put odd key', r.status === 200, r.status);
r = await call('GET', `/${B}/${key}`); ok('get odd key', r.status === 200 && (await r.text()) === 'ui body', r.status);
r = await call('HEAD', `/${B}/${key}`); ok('head', r.status === 200 && r.headers.get('content-length') === '7', r.status);
r = await call('GET', `/${B}`, { query: 'list-type=2&prefix=dir%2F&delimiter=%2F' }); const x = await r.text();
ok('list with encoded query', r.status === 200 && x.includes('<Key>dir/a b+c é 日本.txt</Key>'), `${r.status} ${x.slice(0, 200)}`);
r = await call('GET', '/'); ok('list buckets', r.status === 200 && (await r.text()).includes(`<Name>${B}</Name>`), r.status);
r = await call('GET', '/', { secret: 'wrong' }); ok('wrong secret refused', r.status === 403 && (await r.text()).includes('SignatureDoesNotMatch'), r.status);
r = await call('DELETE', `/${B}/${key}`); ok('delete', r.status === 204, r.status);
r = await call('DELETE', `/${B}`); ok('delete bucket', r.status === 204, r.status);
process.exit(fails ? 1 : 0);
