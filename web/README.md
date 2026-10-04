# Web 前端

基于 Next.js 的企业文档检索问答平台前端。

## 开发

```bash
npm install
npm run dev
```

访问 http://localhost:3000

## API 代理

开发环境通过 `next.config.ts` 中的 `rewrites` 将 `/api/*` 代理到后端 C++ 服务器。

默认代理目标: `http://127.0.0.1:9006/api/:path*`

Set `BACKEND_API_BASE` when building to change the proxy target:

```ts
BACKEND_API_BASE=http://backend:9006 npm run build
```

## 生产构建

```bash
npm run build
npm start
```

The build produces `.next/standalone/server.js`. Run it with Node.js after
copying `public/` and `.next/static/` into the standalone directory. PDF routes
require Python, pypdf, processing scripts and persistent data directories.
They require a running Next.js server; C++ static hosting cannot execute them.
See [the server deployment guide](../deploy/server/README.md).
