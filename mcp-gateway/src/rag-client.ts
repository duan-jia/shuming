import { randomUUID } from 'node:crypto';
import type { ServicePrincipal } from './config.js';

export type SearchMode = 'chunks' | 'parents';

export interface SearchKnowledgeInput {
  query: string;
  top_k: number;
  mode: SearchMode;
}

export interface SearchEvidence {
  doc_id: number;
  filename: string;
  content: string;
  chunk_index?: number;
  score?: number;
  matched_chunks?: number;
  max_score?: number;
}

export interface SearchKnowledgeResult {
  query: string;
  rewritten_query: string;
  query_rewritten: boolean;
  query_aliases: string[];
  rewrite_failure_reason?: string;
  results: SearchEvidence[];
}

export class RagApiError extends Error {
  constructor(
    message: string,
    readonly status?: number,
    readonly retryable = false
  ) {
    super(message);
    this.name = 'RagApiError';
  }
}

interface CachedToken {
  token: string;
  refreshAfter: number;
}

function hasStaticToken(principal: ServicePrincipal): principal is ServicePrincipal & { upstreamBearerToken: string } {
  return 'upstreamBearerToken' in principal && typeof principal.upstreamBearerToken === 'string';
}

function jwtRefreshAfter(token: string): number {
  try {
    const [, payload] = token.split('.');
    if (!payload) return Date.now() + 5 * 60_000;
    const claims = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8')) as { exp?: unknown };
    if (typeof claims.exp === 'number') return Math.max(Date.now(), claims.exp * 1000 - 60_000);
  } catch {
    // A successful login response may use an opaque token. Cache it briefly.
  }
  return Date.now() + 5 * 60_000;
}

export class RagClient {
  private readonly tokenCache = new Map<string, CachedToken>();
  private readonly tokenRequests = new Map<string, Promise<string>>();

  constructor(
    private readonly baseUrl: string,
    private readonly timeoutMs: number
  ) {}

  private async fetchWithTimeout(url: string, init: RequestInit): Promise<Response> {
    try {
      return await fetch(url, { ...init, signal: AbortSignal.timeout(this.timeoutMs) });
    } catch (error) {
      const timedOut = error instanceof Error && (error.name === 'TimeoutError' || error.name === 'AbortError');
      throw new RagApiError(timedOut ? 'knowledge search timed out' : 'knowledge search service is unreachable', undefined, true);
    }
  }

  private async login(principal: ServicePrincipal & { upstreamUsername: string; upstreamPassword: string }): Promise<string> {
    const response = await this.fetchWithTimeout(`${this.baseUrl}/api/auth/login`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ username: principal.upstreamUsername, password: principal.upstreamPassword })
    });
    if (!response.ok) {
      throw new RagApiError('service principal authentication failed', response.status, response.status >= 500);
    }

    let body: { token?: unknown; user?: { tenant_id?: unknown } };
    try {
      body = JSON.parse(await response.text()) as typeof body;
    } catch {
      throw new RagApiError('service principal authentication returned invalid JSON', 502, true);
    }
    if (typeof body.token !== 'string' || body.token.length === 0) {
      throw new RagApiError('service principal authentication returned no token', 502, true);
    }
    if (body.user?.tenant_id !== principal.tenantId) {
      throw new RagApiError('service principal tenant does not match its configured tenant', 502, false);
    }
    this.tokenCache.set(principal.name, { token: body.token, refreshAfter: jwtRefreshAfter(body.token) });
    return body.token;
  }

  private async tokenFor(principal: ServicePrincipal, forceRefresh = false): Promise<string> {
    if (hasStaticToken(principal)) return principal.upstreamBearerToken;
    if (!forceRefresh) {
      const cached = this.tokenCache.get(principal.name);
      if (cached && cached.refreshAfter > Date.now()) return cached.token;
    } else {
      this.tokenCache.delete(principal.name);
    }

    const existing = this.tokenRequests.get(principal.name);
    if (existing) return existing;
    const request = this.login(principal).finally(() => this.tokenRequests.delete(principal.name));
    this.tokenRequests.set(principal.name, request);
    return request;
  }

  private async searchResponse(token: string, input: SearchKnowledgeInput, requestId: string): Promise<Response> {
    return this.fetchWithTimeout(`${this.baseUrl}/api/documents/search`, {
      method: 'POST',
      headers: {
        authorization: `Bearer ${token}`,
        'content-type': 'application/json',
        'x-request-id': requestId
      },
      body: JSON.stringify(input)
    });
  }

  async nativeMcp(principal: ServicePrincipal, path: '/mcp/knowledge' | '/mcp/web',
    body: unknown, protocolVersion?: string, origin?: string): Promise<Response> {
    const send = (token: string) => this.fetchWithTimeout(`${this.baseUrl}${path}`, {
      method: 'POST',
      headers: {
        authorization: `Bearer ${token}`,
        'content-type': 'application/json',
        accept: 'application/json, text/event-stream',
        ...(protocolVersion ? { 'mcp-protocol-version': protocolVersion } : {}),
        ...(origin ? { origin } : {})
      },
      body: JSON.stringify(body)
    });
    let response = await send(await this.tokenFor(principal));
    if (response.status === 401 && !hasStaticToken(principal)) {
      await response.body?.cancel();
      response = await send(await this.tokenFor(principal, true));
    }
    return response;
  }

  async search(principal: ServicePrincipal, input: SearchKnowledgeInput): Promise<SearchKnowledgeResult> {
    const requestId = randomUUID();
    let token = await this.tokenFor(principal);
    let response = await this.searchResponse(token, input, requestId);
    if (response.status === 401 && !hasStaticToken(principal)) {
      token = await this.tokenFor(principal, true);
      response = await this.searchResponse(token, input, requestId);
    }

    const bodyText = await response.text();
    if (!response.ok) {
      let detail = `upstream returned HTTP ${response.status}`;
      try {
        const body = JSON.parse(bodyText) as { error?: unknown };
        if (typeof body.error === 'string' && body.error.length <= 500) detail = body.error;
      } catch {
        // Do not expose arbitrary upstream response bodies.
      }
      throw new RagApiError(detail, response.status, response.status >= 500);
    }

    try {
      return JSON.parse(bodyText) as SearchKnowledgeResult;
    } catch {
      throw new RagApiError('knowledge search returned invalid JSON', 502, true);
    }
  }
}
