import type { Server } from 'node:http';
import { createMcpExpressApp } from '@modelcontextprotocol/express';
import { toNodeHandler } from '@modelcontextprotocol/node';
import type { AuthInfo } from '@modelcontextprotocol/server';
import { createMcpHandler, McpServer } from '@modelcontextprotocol/server';
import type { NextFunction, Request, Response } from 'express';
import * as z from 'zod/v4';
import type { GatewayConfig, ServicePrincipal } from './config.js';
import { findPrincipal } from './config.js';
import { RagApiError, RagClient } from './rag-client.js';

declare global {
  namespace Express {
    interface Request {
      auth?: AuthInfo;
    }
  }
}

function bearerToken(req: Request): string | undefined {
  const header = req.header('authorization');
  const match = header?.match(/^Bearer ([^\s]+)$/i);
  return match?.[1];
}

export function createGateway(config: GatewayConfig) {
  const principalByName = new Map(config.principals.map(principal => [principal.name, principal]));
  const ragClient = new RagClient(config.ragApiBaseUrl, config.ragTimeoutMs);

  const mcpHandler = createMcpHandler(({ authInfo }) => {
    const principal = authInfo ? principalByName.get(authInfo.clientId) : undefined;
    const server = new McpServer(
      { name: 'fast-llm-reasoner', version: '0.1.0' },
      { capabilities: { tools: {} } }
    );

    server.registerTool(
      'search_knowledge',
      {
        title: 'Search laboratory knowledge',
        description: 'Search the authenticated laboratory tenant knowledge base and return evidence. Use the evidence to answer the user; do not invent facts not supported by it.',
        inputSchema: z.object({
          query: z.string().trim().min(1).max(4000).describe('Natural-language search query'),
          top_k: z.number().int().min(1).max(20).default(5).describe('Maximum evidence items'),
          mode: z.enum(['chunks', 'parents']).default('chunks').describe('Return matching chunks or consolidated parent documents')
        }).strict()
      },
      async ({ query, top_k, mode }) => {
        if (!principal) {
          return { isError: true, content: [{ type: 'text', text: 'Authenticated service principal is unavailable.' }] };
        }
        try {
          const result = await ragClient.search(principal, { query, top_k, mode });
          return {
            content: [{ type: 'text', text: JSON.stringify(result) }],
            structuredContent: result
          };
        } catch (error) {
          const message = error instanceof RagApiError ? error.message : 'knowledge search failed';
          return {
            isError: true,
            content: [{ type: 'text', text: message }]
          };
        }
      }
    );
    return server;
  }, { responseMode: 'json' });
  const nodeHandler = toNodeHandler(mcpHandler);

  const app = createMcpExpressApp({ host: config.host, allowedHosts: config.allowedHosts });
  app.get('/healthz', (_req, res) => res.status(200).json({ status: 'ok' }));
  app.use('/mcp', (req: Request, res: Response, next: NextFunction) => {
    const token = bearerToken(req);
    const principal = token ? findPrincipal(token, config.principals) : undefined;
    if (!token || !principal) {
      res.setHeader('WWW-Authenticate', 'Bearer realm="fast-llm-reasoner-mcp"');
      res.status(401).json({ error: 'invalid_token' });
      return;
    }
    req.auth = {
      token,
      clientId: principal.name,
      scopes: ['search:knowledge'],
      extra: { tenantId: principal.tenantId }
    };
    next();
  });
  app.all('/mcp', (req, res) => void nodeHandler(req, res, req.body));

  for (const path of ['/mcp/knowledge', '/mcp/web'] as const) {
    app.post(path, async (req, res) => {
      const principal = req.auth ? principalByName.get(req.auth.clientId) : undefined;
      if (!principal) {
        res.status(401).json({ error: 'invalid_token' });
        return;
      }
      try {
        const response = await ragClient.nativeMcp(principal, path, req.body,
          req.header('mcp-protocol-version'), req.header('origin'));
        const body = await response.text();
        for (const header of ['content-type', 'retry-after']) {
          const value = response.headers.get(header);
          if (value) res.setHeader(header, value);
        }
        res.status(response.status).send(body);
      } catch {
        res.status(502).json({ error: 'MCP upstream is unavailable' });
      }
    });
    app.all(path, (_req, res) => res.status(405).set('Allow', 'POST').end());
  }

  return {
    app,
    close: () => mcpHandler.close(),
    listen(): Server {
      return app.listen(config.port, config.host);
    }
  };
}
