# 原生 MCP：知识库检索与网页搜索

两个 MCP 服务入口运行在同一个 C++ 后端中，共用 muduo HTTP listener、JWT 身份和
有界工具工作线程池。MCP 适配层直接调用工具业务层，不经过 TypeScript 网关，也不向
本机 REST 接口二次发送 HTTP 请求。

```text
外部 Agent / MCP Client
  ├─ /mcp/knowledge → search_knowledge → KnowledgeSearchService → RAG
  └─ /mcp/web       → search_web → WebSearchService → DeepSeek Search API

/mcp 同时提供上述两个工具，适合只配置一个 MCP 连接的客户端。
```

## 入口与协议

| 地址 | 工具 | 用途 |
|---|---|---|
| `/mcp/knowledge` | `search_knowledge` | 独立接入内部知识检索 |
| `/mcp/web` | `search_web` | 独立接入网页搜索 |
| `/mcp` | 两个已获授权的工具 | 统一接入 |

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

推荐通过 `DEEPSEEK_SEARCH_API_KEY` 环境变量提供专用密钥，优先于配置中的
`web_search.api_key`；不会自动使用生成模型的 API key。开启但未配置密钥时，调用返回
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

`ToolService` 是可供进程内 Agent Runtime 和 MCP 共用的工具边界。当前聊天仍保留
已有单次 RAG → 生成流程；本次实现两个 MCP 工具服务入口，不包含自主 Agent Loop。
后续 Runtime 可以直接调用 ToolService，无需对本进程再发 MCP/HTTP 请求。

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
