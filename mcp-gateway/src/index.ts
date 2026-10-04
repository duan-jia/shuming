import { createGateway } from './app.js';
import { loadConfig } from './config.js';

const config = loadConfig();
const gateway = createGateway(config);
const server = gateway.listen();

server.on('listening', () => {
  console.error(`fast-llm-reasoner MCP gateway listening on ${config.host}:${config.port}`);
});

async function shutdown(): Promise<void> {
  server.close();
  await gateway.close();
}

process.once('SIGINT', () => void shutdown());
process.once('SIGTERM', () => void shutdown());
