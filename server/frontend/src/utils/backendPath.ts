/**
 * @file backendPath.ts
 * @brief Map the UI's /api/s3/... paths onto the object store's own routes.
 */

/**
 * @brief Translate the segments after /api/s3/ into a backend path.
 * @param segments - Path segments, e.g. ['objects', 'b', 'dir', 'k'].
 * @returns Backend path (starting with /), or null for an unknown route.
 */
export function backendPath(segments: string[]): string | null {
  const [kind, bucket, ...key] = segments;
  switch (kind) {
    case 'health':
      return segments.length === 1 ? '/health' : null;
    case 'buckets':
      if (segments.length === 1) return '/';
      return segments.length === 2 ? `/${bucket}` : null;
    case 'list':
      return segments.length === 2 ? `/${bucket}` : null;
    case 'objects':
      return bucket && key.length > 0
        ? `/${bucket}/${key.join('/')}`
        : null;
    default:
      return null;
  }
}

/** @brief Backend base URL, read per request so it can change at runtime. */
export function backendBase(): string {
  return (
    process.env.S3_BACKEND_URL ||
    process.env.BACKEND_URL ||
    'http://backend:9000'
  ).replace(/\/+$/, '');
}
