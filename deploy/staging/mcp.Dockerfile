FROM mirror.ccs.tencentyun.com/library/node:24-bookworm-slim AS build

WORKDIR /app
COPY mcp-gateway/package.json mcp-gateway/package-lock.json ./
RUN npm ci
COPY mcp-gateway/tsconfig.json ./
COPY mcp-gateway/src ./src
RUN npm run build

FROM mirror.ccs.tencentyun.com/library/node:24-bookworm-slim AS runtime

ENV NODE_ENV=production
WORKDIR /app
COPY --from=build /app/package.json /app/package-lock.json ./
# Staging retains the MCP client dev dependency solely for the deployed E2E probe.
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
COPY deploy/staging/e2e.mjs ./e2e.mjs

USER node
EXPOSE 3100
CMD ["node", "dist/index.js"]
