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

默认代理目标: `http://127.0.0.1:8080/api/:path*`

如果后端运行在其他端口, 修改 `next.config.ts`:

```ts
destination: "http://127.0.0.1:9006/api/:path*",
```

## 生产构建

```bash
npm run build
npm start
```

静态文件也可由 C++ 后端直接托管 (`--static` 参数, 需先构建 `web/.next/standalone`)。
