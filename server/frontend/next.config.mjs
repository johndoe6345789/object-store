import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// The backend URL is NOT configured here: /api/s3/* is proxied by
// src/app/api/s3/[...path]/route.ts, which reads S3_BACKEND_URL at runtime.

/** @type {import('next').NextConfig} */
const nextConfig = {
  basePath: process.env.NEXT_BASE_PATH || "",
  output: "standalone",
  sassOptions: {
    silenceDeprecations: ["legacy-js-api"],
  },
  turbopack: { root: __dirname },
};

export default nextConfig;
