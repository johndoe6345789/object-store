# syntax=docker/dockerfile:1.7
# frontend -- standalone production build. Self-contained: the build context
# is this directory only (server/frontend); nothing is pulled from sibling
# repos.
#
#   docker build -t object-store-frontend server/frontend
#   docker run -p 3000:3000 -e S3_BACKEND_URL=http://backend:9000 object-store-frontend

# -- Stage 1: deps --------------------------
FROM node:22-alpine AS deps
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund

# -- Stage 2: build -------------------------
FROM node:22-alpine AS build
WORKDIR /app
# basePath is the one setting Next bakes into the build.
ARG NEXT_BASE_PATH=
ENV NEXT_BASE_PATH=${NEXT_BASE_PATH}
ENV NEXT_TELEMETRY_DISABLED=1
COPY --from=deps /app/node_modules ./node_modules
COPY . .
RUN mkdir -p public && npm run build

# -- Stage 3: runner -------------------------
FROM node:22-alpine AS runner
WORKDIR /app
ENV NODE_ENV=production
ENV NEXT_TELEMETRY_DISABLED=1
ENV PORT=3000
ENV HOSTNAME=0.0.0.0
# Read on every request (src/app/api/s3/[...path]/route.ts), so it can be set
# at `docker run` time.
ENV S3_BACKEND_URL=http://backend:9000
COPY --from=build --chown=node:node /app/.next/standalone ./
COPY --from=build --chown=node:node /app/.next/static ./.next/static
COPY --from=build --chown=node:node /app/public ./public
USER node
EXPOSE 3000
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s \
  CMD node -e "fetch('http://127.0.0.1:'+process.env.PORT+'/login').then(r=>process.exit(r.status<500?0:1),()=>process.exit(1))"
CMD ["node", "server.js"]
