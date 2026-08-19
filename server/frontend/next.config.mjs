import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
// Siblings under one common parent, matching this org's local-dev layout
// (all micro-repos checked out flat next to each other) -- not "shared",
// which predates the split and no longer exists anywhere. @metabuilder/scss
// is `private: true` (can't publish to Verdaccio, unlike @metabuilder/m3),
// so CI clones github.com/johndoe6345789/scss and points
// METABUILDER_SCSS_DIR at it; local dev just needs the sibling checkout.
const siblingRoot = path.resolve(__dirname, "../../..");
const scssRoot = process.env.METABUILDER_SCSS_DIR || path.join(siblingRoot, "scss");

const backend = process.env.BACKEND_URL || "http://localhost:9000";

/** @type {import('next').NextConfig} */
const nextConfig = {
  basePath: process.env.NEXT_BASE_PATH || "",
  output: "standalone",
  transpilePackages: ["@metabuilder/m3"],
  sassOptions: {
    silenceDeprecations: ["legacy-js-api"],
    includePaths: [scssRoot, path.join(scssRoot, "m3-scss")],
    loadPaths: [path.join(scssRoot, "m3-scss"), scssRoot],
  },
  turbopack: { root: siblingRoot },
  async rewrites() {
    return [
      {
        source: "/api/s3/health",
        destination: `${backend}/health`,
      },
      {
        source: "/api/s3/buckets",
        destination: `${backend}/`,
      },
      {
        source: "/api/s3/buckets/:bucket",
        destination: `${backend}/:bucket`,
      },
      {
        source: "/api/s3/list/:bucket",
        destination: `${backend}/list/:bucket`,
      },
      {
        source: "/api/s3/objects/:bucket/:key*",
        destination: `${backend}/:bucket/:key*`,
      },
    ];
  },
};

export default nextConfig;
