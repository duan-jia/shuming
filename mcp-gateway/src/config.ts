import { createHash, timingSafeEqual } from 'node:crypto';
import * as z from 'zod/v4';

const servicePrincipalBaseSchema = z.object({
  name: z.string().trim().min(1),
  tokenSha256: z.string().regex(/^[0-9a-f]{64}$/i),
  tenantId: z.number().int().positive()
});

const servicePrincipalSchema = z.union([
  servicePrincipalBaseSchema.extend({
    upstreamBearerToken: z.string().min(1),
    upstreamUsername: z.never().optional(),
    upstreamPassword: z.never().optional()
  }),
  servicePrincipalBaseSchema.extend({
    upstreamBearerToken: z.never().optional(),
    upstreamUsername: z.string().trim().min(1),
    upstreamPassword: z.string().min(1)
  })
]);

const servicePrincipalsSchema = z.array(servicePrincipalSchema).min(1);

export type ServicePrincipal = z.infer<typeof servicePrincipalSchema>;

export interface GatewayConfig {
  host: string;
  port: number;
  allowedHosts: string[];
  ragApiBaseUrl: string;
  ragTimeoutMs: number;
  principals: ServicePrincipal[];
}

function integerEnv(value: string | undefined, fallback: number, name: string): number {
  if (value === undefined || value === '') return fallback;
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`${name} must be a positive integer`);
  }
  return parsed;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): GatewayConfig {
  const host = env.MCP_HOST ?? '127.0.0.1';
  const port = integerEnv(env.MCP_PORT, 3100, 'MCP_PORT');
  const ragTimeoutMs = integerEnv(env.RAG_TIMEOUT_MS, 10_000, 'RAG_TIMEOUT_MS');
  const ragApiBaseUrl = new URL(env.RAG_API_BASE_URL ?? 'http://127.0.0.1:9006');
  if (!['http:', 'https:'].includes(ragApiBaseUrl.protocol)) {
    throw new Error('RAG_API_BASE_URL must use http or https');
  }

  let rawPrincipals: unknown;
  try {
    rawPrincipals = JSON.parse(env.SERVICE_PRINCIPALS_JSON ?? '[]');
  } catch (error) {
    throw new Error(`SERVICE_PRINCIPALS_JSON is not valid JSON: ${String(error)}`);
  }
  const principals = servicePrincipalsSchema.parse(rawPrincipals);
  const names = new Set<string>();
  const hashes = new Set<string>();
  for (const principal of principals) {
    principal.tokenSha256 = principal.tokenSha256.toLowerCase();
    if (names.has(principal.name)) throw new Error(`duplicate service principal name: ${principal.name}`);
    if (hashes.has(principal.tokenSha256)) throw new Error('duplicate service principal token hash');
    names.add(principal.name);
    hashes.add(principal.tokenSha256);
  }

  const allowedHosts = (env.MCP_ALLOWED_HOSTS ?? `localhost,127.0.0.1,[::1]`)
    .split(',')
    .map(value => value.trim())
    .filter(Boolean);
  if (host !== '127.0.0.1' && host !== 'localhost' && env.MCP_ALLOWED_HOSTS === undefined) {
    throw new Error('MCP_ALLOWED_HOSTS is required when MCP_HOST is not loopback');
  }

  return {
    host,
    port,
    allowedHosts,
    ragApiBaseUrl: ragApiBaseUrl.toString().replace(/\/$/, ''),
    ragTimeoutMs,
    principals
  };
}

export function sha256Token(token: string): string {
  return createHash('sha256').update(token, 'utf8').digest('hex');
}

export function findPrincipal(token: string, principals: readonly ServicePrincipal[]): ServicePrincipal | undefined {
  const actual = createHash('sha256').update(token, 'utf8').digest();
  for (const principal of principals) {
    const expected = Buffer.from(principal.tokenSha256, 'hex');
    if (expected.length === actual.length && timingSafeEqual(expected, actual)) return principal;
  }
  return undefined;
}
