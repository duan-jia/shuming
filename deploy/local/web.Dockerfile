FROM node:24-slim AS build
WORKDIR /app
ENV NEXT_TELEMETRY_DISABLED=1
COPY web/package.json web/package-lock.json ./
ARG NPM_REGISTRY=https://registry.npmjs.org
RUN npm ci --registry=${NPM_REGISTRY}
COPY web/ ./
ARG BACKEND_API_BASE=http://backend:9006
ENV BACKEND_API_BASE=${BACKEND_API_BASE}
RUN npm run build

FROM node:24-slim AS runtime-base
ARG DEBIAN_MIRROR=deb.debian.org
RUN sed -i "s|deb.debian.org|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 update && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 install -y --no-install-recommends python3 python3-pypdf ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app/web
ENV NODE_ENV=production NEXT_TELEMETRY_DISABLED=1 HOSTNAME=0.0.0.0 PORT=3000 \
    PROJECT_ROOT=/app BACKEND_API_BASE=http://backend:9006
FROM runtime-base AS runtime
COPY --from=build --chown=node:node /app/.next/standalone ./
COPY --from=build --chown=node:node /app/.next/static ./.next/static
COPY --from=build --chown=node:node /app/public ./public
COPY --chown=node:node scripts/ /app/scripts/
USER node
EXPOSE 3000
CMD ["node", "server.js"]
