import { createHash, createHmac } from 'node:crypto';

/** Percent-encode per SigV4 (unreserved kept, uppercase hex, '/' optional). */
export function uriEncode(s: string, keepSlash = false): string {
  let out = '';
  for (const b of Buffer.from(s, 'utf8')) {
    const c = String.fromCharCode(b);
    if (/[A-Za-z0-9\-_.~]/.test(c) || (keepSlash && c === '/')) out += c;
    else out += '%' + b.toString(16).toUpperCase().padStart(2, '0');
  }
  return out;
}

/** Canonical URI for a decoded path (each byte encoded once). */
export function canonicalUri(decodedPath: string): string {
  return uriEncode(decodedPath || '/', true);
}

/** Canonical query from a raw query string (with or without leading '?'). */
export function canonicalQuery(rawQuery: string): string {
  const params = new URLSearchParams(rawQuery.replace(/^\?/, ''));
  const pairs: [string, string][] = [];
  for (const [k, v] of params) pairs.push([uriEncode(k), uriEncode(v)]);
  pairs.sort((a, b) =>
    a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0,
  );
  return pairs.map(([k, v]) => `${k}=${v}`).join('&');
}

export interface SignInput {
  method: string;
  /** Decoded (unencoded) request path. */
  path: string;
  /** Raw query string, may be empty. */
  query?: string;
  /** Host header value as sent upstream (port only if non-default). */
  host: string;
  accessKey: string;
  secretKey: string;
  region: string;
  /** Hex payload hash or UNSIGNED-PAYLOAD. */
  payloadHash?: string;
  /** Additional headers to sign (e.g. range). */
  extraHeaders?: Record<string, string>;
  /** Injectable clock. */
  now?: Date;
}

export interface SignResult {
  authorization: string;
  amzDate: string;
  payloadHash: string;
  canonicalUri: string;
  canonicalQuery: string;
}

const sha256hex = (s: string): string =>
  createHash('sha256').update(s, 'utf8').digest('hex');
const hmac = (k: Buffer | string, s: string): Buffer =>
  createHmac('sha256', k).update(s, 'utf8').digest();

/** Sign a request with AWS SigV4 (header auth, service s3). */
export function signV4(i: SignInput): SignResult {
  const payloadHash = i.payloadHash ?? 'UNSIGNED-PAYLOAD';
  const amzDate = (i.now ?? new Date())
    .toISOString()
    .replace(/[-:]/g, '')
    .replace(/\.\d+Z$/, 'Z');
  const day = amzDate.slice(0, 8);
  const hdrs: Record<string, string> = {};
  for (const [k, v] of Object.entries(i.extraHeaders ?? {})) {
    hdrs[k.toLowerCase()] = v.trim().replace(/\s+/g, ' ');
  }
  hdrs['host'] = i.host;
  hdrs['x-amz-content-sha256'] = payloadHash;
  hdrs['x-amz-date'] = amzDate;
  const names = Object.keys(hdrs).sort();
  const signedHeaders = names.join(';');
  const cUri = canonicalUri(i.path);
  const cQuery = canonicalQuery(i.query ?? '');
  const canonical = [
    i.method.toUpperCase(),
    cUri,
    cQuery,
    names.map((n) => `${n}:${hdrs[n]}\n`).join(''),
    signedHeaders,
    payloadHash,
  ].join('\n');
  const scope = `${day}/${i.region}/s3/aws4_request`;
  const toSign = ['AWS4-HMAC-SHA256', amzDate, scope, sha256hex(canonical)].join(
    '\n',
  );
  let key = hmac('AWS4' + i.secretKey, day);
  key = hmac(key, i.region);
  key = hmac(key, 's3');
  key = hmac(key, 'aws4_request');
  const signature = createHmac('sha256', key).update(toSign).digest('hex');
  return {
    authorization:
      `AWS4-HMAC-SHA256 Credential=${i.accessKey}/${scope}, ` +
      `SignedHeaders=${signedHeaders}, Signature=${signature}`,
    amzDate,
    payloadHash,
    canonicalUri: cUri,
    canonicalQuery: cQuery,
  };
}
