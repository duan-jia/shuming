> Compatibility gateway: new installations can use the C++ native MCP endpoints directly.
> See [native MCP](../docs/native-mcp.md). This gateway remains available for existing
> deployments and its service-principal token mapping; it is not required by native MCP.

# MCP Gateway

Internal remote MCP gateway for the `fast-llm-reasoner` RAG service. It exposes one tool:

- `search_knowledge(query, top_k = 5, mode = "chunks")`

The caller cannot provide a tenant ID. An external bearer token selects a configured service
principal; the gateway then forwards that principal's tenant-bound JWT to the C++
`POST /api/documents/search` endpoint.

## Configure

```bash
cd mcp-gateway
npm ci
cp .env.example .env
```

Environment variables:

| Variable | Meaning |
|---|---|
| `MCP_HOST` | Bind address; defaults to `127.0.0.1` |
| `MCP_PORT` | Listen port; defaults to `3100` |
| `MCP_ALLOWED_HOSTS` | Comma-separated host allowlist; required for non-loopback binds |
| `RAG_API_BASE_URL` | Base URL of the C++ service |
| `RAG_TIMEOUT_MS` | Upstream timeout; defaults to 10 seconds |
| `SERVICE_PRINCIPALS_JSON` | Array of service-principal mappings |

Do not commit credentials. `tokenSha256` is the lowercase SHA-256 digest of the bearer token
accepted from an MCP client. For each principal, configure exactly one upstream authentication
mode:

- Recommended: `upstreamUsername` and `upstreamPassword` for a dedicated service-account user.
  The gateway calls `/api/auth/login`, verifies that the returned `tenant_id` matches the configured
  `tenantId`, caches the JWT, refreshes it before expiry, and retries once after an upstream `401`.
- Migration-only: `upstreamBearerToken`, a pre-issued tenant-bound JWT that must be rotated manually.

The MCP tool never accepts a tenant ID. Tenant scope comes from the authenticated C++ service
account and is checked against the configured principal mapping before any search is sent.

## Run

```bash
set -a; . ./.env; set +a
npm run build
npm start
```

The MCP endpoint is `http://HOST:PORT/mcp`; unauthenticated health checks use `/healthz`.
Terminate TLS at the reverse proxy and expose only HTTPS outside the host.

## Verify

```bash
npm test
npm run typecheck
npm run build
```
