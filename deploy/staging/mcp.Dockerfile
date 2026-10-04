ARG NODE_IMAGE=node:24-bookworm-slim
FROM ${NODE_IMAGE} AS build
WORKDIR /app
COPY mcp-gateway/package.json mcp-gateway/package-lock.json ./
RUN npm ci
COPY mcp-gateway/tsconfig.json ./
COPY mcp-gateway/src ./src
RUN npm run build

FROM ${NODE_IMAGE} AS runtime
ENV NODE_ENV=production
WORKDIR /app
COPY --from=build /app/package.json /app/package-lock.json ./
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
COPY deploy/staging/e2e.mjs ./e2e.mjs
USER node
EXPOSE 3100
CMD ["node", "dist/index.js"]
