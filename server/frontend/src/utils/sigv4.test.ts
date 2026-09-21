import { test } from 'node:test';
import assert from 'node:assert/strict';
import { signV4, canonicalUri, canonicalQuery } from './sigv4.ts';

const base = {
  accessKey: 'AKIAIOSFODNN7EXAMPLE',
  secretKey: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
  region: 'us-east-1',
  host: 'examplebucket.s3.amazonaws.com',
  now: new Date('2013-05-24T00:00:00Z'),
};
const EMPTY = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855';

test('AWS docs example: GET object with Range', () => {
  const r = signV4({
    ...base,
    method: 'GET',
    path: '/test.txt',
    payloadHash: EMPTY,
    extraHeaders: { Range: 'bytes=0-9' },
  });
  assert.equal(r.amzDate, '20130524T000000Z');
  assert.match(r.authorization, /SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,/);
  assert.match(
    r.authorization,
    /Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41$/,
  );
  assert.match(
    r.authorization,
    /^AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE\/20130524\/us-east-1\/s3\/aws4_request, /,
  );
});

test('AWS docs example: GET with query (?lifecycle)', () => {
  const r = signV4({
    ...base,
    method: 'GET',
    path: '/',
    query: '?lifecycle',
    payloadHash: EMPTY,
  });
  assert.equal(r.canonicalQuery, 'lifecycle=');
  assert.match(r.authorization, /SignedHeaders=host;x-amz-content-sha256;x-amz-date,/);
  assert.match(
    r.authorization,
    /Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543$/,
  );
});

test('defaults to UNSIGNED-PAYLOAD', () => {
  const r = signV4({ ...base, method: 'PUT', path: '/b/k' });
  assert.equal(r.payloadHash, 'UNSIGNED-PAYLOAD');
});

test('canonical URI encodes once, keeps slashes', () => {
  assert.equal(canonicalUri('/b/a b/c+d.txt'), '/b/a%20b/c%2Bd.txt');
  assert.equal(canonicalUri('/b/é/日'), '/b/%C3%A9/%E6%97%A5');
  assert.equal(canonicalUri('/b/a%20b'), '/b/a%2520b');
  assert.equal(canonicalUri('/b/a-b_c.d~e/x'), '/b/a-b_c.d~e/x');
  assert.equal(canonicalUri(''), '/');
});

test('canonical query decodes, re-encodes and sorts', () => {
  assert.equal(
    canonicalQuery('prefix=a b/c&list-type=2&delimiter=%2F'),
    'delimiter=%2F&list-type=2&prefix=a%20b%2Fc',
  );
  assert.equal(canonicalQuery('k=a+b&k=a%2Bb&k=1'), 'k=1&k=a%20b&k=a%2Bb');
  assert.equal(canonicalQuery('prefix=%C3%A9'), 'prefix=%C3%A9');
  assert.equal(canonicalQuery(''), '');
});
