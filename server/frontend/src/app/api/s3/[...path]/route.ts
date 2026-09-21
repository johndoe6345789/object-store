import { backendPath, backendBase } from '@/utils/backendPath';
import { signV4, canonicalUri, canonicalQuery } from '@/utils/sigv4';

// Read S3_BACKEND_URL when a request arrives, not at build time, so one image
// works against any backend. (next.config rewrites are baked into the build.)
export const dynamic = 'force-dynamic';

const FORWARD_REQUEST = ['content-type', 'content-length'];
const FORWARD_RESPONSE = [
  'content-type',
  'content-length',
  'etag',
  'last-modified',
  'x-amz-bucket-region',
  'location',
];

async function proxy(
  req: Request,
  ctx: { params: Promise<{ path: string[] }> },
): Promise<Response> {
  const { path } = await ctx.params;
  const target = backendPath(path);
  if (!target) return new Response('Not found', { status: 404 });

  const headers = new Headers();
  for (const h of FORWARD_REQUEST) {
    const v = req.headers.get(h);
    if (v) headers.set(h, v);
  }
  const accessKey = req.headers.get('x-s3-access-key');
  const secretKey = req.headers.get('x-s3-secret-key');
  const base = new URL(`${backendBase()}/`);
  const search = new URL(req.url).search;
  const query = canonicalQuery(search);
  if (accessKey && secretKey) {
    // base.host omits default ports, matching what undici sends.
    const signed = signV4({
      method: req.method,
      path: target,
      query: search,
      host: base.host,
      accessKey,
      secretKey,
      region: process.env.S3_REGION || 'us-east-1',
    });
    headers.set('x-amz-date', signed.amzDate);
    headers.set('x-amz-content-sha256', signed.payloadHash);
    headers.set('authorization', signed.authorization);
  }
  const hasBody = req.method !== 'GET' && req.method !== 'HEAD';
  let upstream: Response;
  try {
    upstream = await fetch(`${backendBase()}${canonicalUri(target)}${query ? `?${query}` : ''}`, {
      method: req.method,
      headers,
      body: hasBody ? req.body : undefined,
      // Streamed both ways: uploads are never buffered here.
      // @ts-expect-error duplex is required by Node's fetch for streams
      duplex: hasBody ? 'half' : undefined,
      redirect: 'manual',
    });
  } catch {
    return new Response('Object store unreachable', { status: 502 });
  }
  const out = new Headers();
  for (const h of FORWARD_RESPONSE) {
    const v = upstream.headers.get(h);
    if (v) out.set(h, v);
  }
  return new Response(upstream.body, { status: upstream.status, headers: out });
}

export {
  proxy as GET,
  proxy as HEAD,
  proxy as PUT,
  proxy as POST,
  proxy as DELETE,
};
