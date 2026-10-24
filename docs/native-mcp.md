# 原生 MCP：知识库检索与网页搜索

两个 MCP 服务入口运行在同一个 C++ 后端中，共用 muduo HTTP listener、JWT 身份和
有界工具工作线程池。MCP 适配层直接调用工具业务层，不经过 TypeScript 网关，也不向
本机 REST 接口二次发送 HTTP 请求。

```text
外部 Agent / MCP Client
  ├─ /mcp/knowledge → search_knowledge / fetch_knowledge → RAG / source PDF
  └─ /mcp/web       → search_web / fetch_web → DeepSeek Search / Fetch API

/mcp 同时提供上述四个工具，适合只配置一个 MCP 连接的客户端。
```

## 入口与协议

| 地址 | 工具 | 用途 |
|---|---|---|
| `/mcp/knowledge` | `search_knowledge`, `fetch_knowledge` | 独立接入内部知识检索 |
| `/mcp/web` | `search_web`, `fetch_web` | 独立接入网页搜索 |
| `/mcp` | 四个已获授权的工具 | 统一接入 |

使用无会话的 Streamable HTTP，支持 `2025-03-26`、`2025-06-18`、`2025-11-25`。
POST 返回 JSON；通知返回空的 202；GET 不开启服务器推送流，返回 405；DELETE
返回 405，因为不维护可删除的 MCP session。支持初始化、ping、工具发现和工具调用。
不提供旧版 HTTP+SSE transport、持久 session、任务恢复或服务器主动请求。

客户端每个 POST 都应发送：

```http
Authorization: Bearer <后端 JWT>
Content-Type: application/json
Accept: application/json, text/event-stream
MCP-Protocol-Version: 2025-11-25
```

初始化时按标准协商协议版本。未发送协议版本头时兼容回退至 `2025-03-26`。
官方协议参考：[Streamable HTTP](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)。

## 身份与权限

复用 `/api/auth/login` 签发的 JWT，不新增用户库。外部 Agent 使用对应租户的专用后端
账号取得 JWT，并在过期后重新登录。原 TS 网关的 opaque bearer token 不能直接用于
原生入口；迁移时需改用后端 JWT，并由客户端管理刷新。

- 知识库租户取自已验证的 JWT，工具参数不接受 `tenant_id`。
- 知识库仍只返回公共文档和当前租户可见文档。
- 网页搜索默认关闭；显式开启后，管理员可使用。
- `mcp.allow_user_web_search=true` 才允许普通用户使用网页搜索。工具发现也按权限过滤。
- 每个工具按用户独立限流，网页搜索另有限制上游并发。
- 非浏览器 MCP 客户端通常不发送 Origin；发送了 Origin 就必须匹配
  `mcp.allowed_origins` 中的完整 origin，否则返回 403。默认空列表拒绝全部带 Origin 的请求。

本机测试使用 loopback。对外部署仍应通过现有反向代理提供 HTTPS，并将相应 `/mcp`
路径转发至后端；不要将 MCP 请求转交前端的聊天 SSE 处理器。

## 配置与启用网页搜索

合并 `config.example.json` 中新增配置到本地配置，不覆盖现有账号、数据库或索引设置：

```json
{
  "web_search": {
    "enabled": true,
    "fetch_endpoint": "https://api.deepseek.com/v1/tools/fetch",
    "endpoint": "https://api.deepseek.com/v1/tools/search",
    "api_key": "",
    "timeout_seconds": 15,
    "max_concurrency": 2,
    "ca_bundle": "/etc/ssl/certs/ca-certificates.crt"
  },
  "mcp": {
    "allowed_origins": [],
    "allow_user_web_search": false,
    "worker_threads": 4,
    "queue_limit": 64,
    "knowledge_requests_per_minute": 60,
    "web_requests_per_minute": 20
  }
}
```

问答、搜索和 PDF 处理可以共用 `DEEPSEEK_API_KEY`。搜索优先使用非空的
`DEEPSEEK_SEARCH_API_KEY`，其次为 `web_search.api_key`，未单独配置时复用
`DEEPSEEK_API_KEY` 或 `remote_engine.api_key`。空环境变量不会覆盖已有配置。
开启但未配置密钥时，调用返回
MCP 工具执行错误。默认关闭时不会在工具发现中展示 `search_web`。

搜索使用 DeepSeek 独立的 Search API，不需要再次调用聊天模型。支持：

```json
{
  "query": "RAG hybrid retrieval site:arxiv.org",
  "count": 5,
  "need_content": false,
  "ori_question": "有哪些相关研究？"
}
```

`query` 和可选的 `ori_question` 为 1～128 个 Unicode 字符，`count` 为 1～50。
返回 query 与 results，保留来源标题、URL、摘要、可选正文、日期和分数。
搜索词会发送给外部供应商，内部知识库检索结果不会自动发送过去。
官方字段参考：[DeepSeek Search](https://api-docs.deepseek.com/zh-cn/api/search/)。

出站 HTTPS 校验服务器证书，不跟随重定向；请求有连接、读写超时、正文读取期限和
2 MiB 响应大小限制。仅为本地测试允许 `http://127.0.0.1:<port>` 上游，工具参数不能
覆盖 endpoint 或 API key。上游故障返回 `isError: true`，不会把上游错误正文或密钥
转交调用者。

## 网络与执行线程

```text
muduo I/O EventLoop
  → HTTP 解析
  → MCP 路径分发
  → 有界工作线程池
      → 身份验证 / MCP 消息处理
      → ToolService
          ├─ 本地 RAG 函数调用
          └─ cpp-httplib 同步 HTTPS（仅占工作线程）
  → 将响应投递回连接所属的 EventLoop
  → HTTP JSON 响应
```

入站连接统一由 muduo 管理，按 `http_threads` 分配 I/O 线程。每条连接的 MCP 请求
按顺序处理，支持 keep-alive 和已缓冲的后续请求。队列满时立即返回 HTTP 503 与
Retry-After，不阻塞 I/O 线程等待队列空间。

DeepSeek 出站连接由 cpp-httplib 管理，不自动进入 muduo 的 epoll。本次未引入
异步出站 HTTP 客户端；使用工作线程隔离阻塞。RAG 内部已有锁，多个请求可能在
索引/嵌入阶段串行，并不保证检索计算并行。

## 与聊天、旧网关的关系

`ToolService` 是进程内 Agent Runtime 和 MCP 共用的工具边界。远程模型聊天默认运行
自主工具调用循环，直接调用 ToolService，无需对本进程再发 MCP/HTTP 请求。本地模型
暂保留原有生成流程。循环、预算、取消与流式事件见 [Agent Runtime](agent-runtime.md)。

`mcp-gateway/` 暂保留为旧客户端的兼容路径，现有 staging 部署不会被自动替换。
迁移调用方到原生入口并验证凭据后，可停止单独的 TS 网关。没有数据库迁移要求。

## 验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF
cmake --build build --target fast-llm-reasoner fast-llm-cli native-mcp-test-server knowledge-search-service-test -j2
./build/bin/knowledge-search-service-test
python3 tests/native_mcp_e2e.py --server-command build/bin/native-mcp-test-server -v
```

测试启动真实 muduo/JWT/MCP 服务、模拟知识库和本地搜索供应商，不访问生产数据库、
模型或真实付费 API。覆盖认证、租户隔离、接口工具隔离、错误处理、Unicode 参数、
上游超时/响应大小、连接复用、队列背压和慢请求时的网络响应。

可额外运行官方 SDK 客户端（Node 20+，依赖版本使用现有网关的 lockfile）：

```bash
cd mcp-gateway && npm ci && cd ..
MCP_SDK_ROOT="$PWD/mcp-gateway/node_modules/@modelcontextprotocol/client" \
  python3 tests/native_mcp_e2e.py \
    --server-command build/bin/native-mcp-test-server \
    --sdk-command 'node tests/native_mcp_sdk.mjs' -v
```

真实 DeepSeek 可用性和计费权限需在部署环境配置密钥后验证；模拟上游测试不证明
账号已开通实际搜索权限。


## Fetch：网页正文和源论文

两个 MCP 入口各提供 search + fetch；统一入口提供全部四个工具。fetch 与对应的 search
使用相同的租户、网页权限策略，分别按用户限流；网页 search/fetch 共用供应商并发限制。
旧的 `mcp-gateway/` 不会自动增加这些工具，调用方需接入原生 C++ MCP。

### 网页

```json
{"name":"fetch_web","arguments":{"url":"https://example.org/article"}}
```

调用服务器配置的 `web_search.fetch_endpoint`（默认 DeepSeek `/v1/tools/fetch`），返回
`url`、`title`、`content`（Markdown）。应用自身不连接工具传入的目标地址。只接受无凭据的
HTTP(S) URL，不允许工具覆盖供应商地址或密钥。上游正文最多 2 MiB，超时、取消、并发和
证书策略与搜索一致。URL 会交给外部供应商，因此不要传入私有文件地址或带密钥的链接。
参考：[DeepSeek Fetch](https://api-docs.deepseek.com/api/fetch/)。

### 知识库

```json
{"name":"fetch_knowledge","arguments":{"doc_id":123,"representation":"text","start_page":1,"page_count":3}}
```

- `text`（默认）：从源 PDF 提取原始文字，不返回模型生成的论文卡片。每次最多选 3 页，
  每段最多 4000 Unicode 字符。先跟随 `next_offset`，保持相同 `start_page/page_count`；
  `next_offset=null` 后才跟随 `next_page`，并将 offset 重置为 0。页码是从 1 开始的 PDF 页序号。
- `pages`：渲染选中页面为最长边 1600 像素的 PNG，作为 MCP `image` 内容返回。
  `structuredContent` 仅放文档和页面元数据，不把 Base64 塞进文本。
- `source`：返回源 PDF 文件名、大小、短期签名下载 URL 和 Unix 秒单位的 `expires_at`。
  支持 resource_link 的协议版本另附 PDF 资源链接。下载链接无需额外 Bearer，持有链接者
  可在有效期内下载这份文件；不要公开分享。链接绑定 doc_id、调用租户、有效期和存储文件名。
  下载时重新检查文档可见性，文档删除或不再可见后链接失效。

`parents` 仍表示完整的入库 Markdown。现有 PDF 上传流程生成的是论文卡片；深入阅读原文
应使用 fetch，而不是把卡片当作完整论文正文。缺少文本层时 `text_available=false`，可改读
页面图片；本实现没有自动 OCR。没有源 PDF 的 Markdown 文档不能通过此工具读取原 PDF。

新增配置（完整默认值见 config.example.json）：

```json
{
  "knowledge_fetch": {
    "pdf_directory": "data/pdfs",
    "reader_script": "scripts/read_pdf.py",
    "python": "python3",
    "public_base_url": "https://knowledge.example.org",
    "link_ttl_seconds": 300,
    "timeout_seconds": 20,
    "max_concurrency": 2
  },
  "remote_engine": {"supports_images": false}
}
```

后端运行环境需要 Python 3 和 Poppler（`pdfinfo`、`pdftotext`、`pdftoppm`，Ubuntu 包
`poppler-utils`）。本地和 staging 后端镜像已包含安装配置及读取脚本。后端必须以只读方式
访问与上传服务相同的源 PDF 目录；例如上传服务使用 `/data/pdfs` 时，为后端挂载同一目录，
并将 `knowledge_fetch.pdf_directory` 设为 `/data/pdfs`。不会修改现有 PDF 或数据库记录。

读取文件大小上限 50 MiB，拒绝路径穿越和源文件符号链接；PDF 处理在有内存、时间、文件
输出大小和并发限制的子进程中执行，不经过 shell。页面处理与下载在有界 worker 池执行。
下载使用 `/api/knowledge/download`，需在反向代理中转发到 C++ 后端（Next.js 的通用 API
代理也可转发）。`public_base_url` 应配置为调用方可访问的 HTTPS 站点根地址；留空时只返回
相对 URL，客户端必须以 MCP 所在站点为基准解析。服务不信任请求 Host 来生成下载地址。
签名密钥优先使用 `KNOWLEDGE_DOWNLOAD_SECRET` 或 `knowledge_fetch.signing_secret`，未配置
时复用 JWT 签名密钥。多个后端副本须使用相同密钥及源文件存储。

内置 Agent 直接复用 ToolService。只有显式配置 `remote_engine.supports_images=true` 时，
才向模型开放 `pages` 模式，并在完成整批工具响应后附加带来源说明的 user 图片消息。
请先确认所选模型及实际代理端点支持图片；此开关不会自动更换模型。纯文本模型仍能读取
原始正文和生成下载链接。图片不计入文本字节截断，输入预算暂按每张 4096 tokens 保守预留，
最终计费仍取供应商实际 usage。附件仅在当前 Agent 请求内保留，后续用户轮次需要重新 fetch。

验证命令仍使用上述 native MCP 和 Agent E2E；测试覆盖真实 PDF 正文分页、空白页面、PNG
渲染、跨租户拒绝、签名链接完整文件下载、篡改和过期拒绝，以及模型下一轮实际收到图片。
这些本地测试使用模拟 DeepSeek 服务，不证明真实账号已开通 Fetch 或视觉输入。
