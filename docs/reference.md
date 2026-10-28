# 配置与接口参考

[返回首页](../README.md) · [模型服务](../deploy/models/README.md)

保留原README中的结构、API和数据库说明；以源码与SQL为准。品牌名称统一为shuming，旧可执行文件名称仅用于兼容。

## 项目结构

```
shuming/
├── CMakeLists.txt              # 顶层 CMake (FetchContent 拉取依赖)
├── config.example.json         # 配置模板 (进 git)
├── config.json                 # 实际配置 (gitignore)
├── sql/init.sql                # 数据库初始化 (9 张表)
├── models/                     # GGUF 模型文件 (gitignore)
├── data/
│   ├── pdfs/                  # 上传的 PDF 源文件
│   ├── processed/             # 大模型生成的 Markdown 与 JSON
│   └── rag/                   # FAISS + BM25 索引文件
├── engine/
│   ├── CMakeLists.txt
│   ├── include/                # 头文件
│   │   ├── server/             # HTTP 服务器, SSE
│   │   ├── persistence/        # MySQL 池, Token 配额
│   │   ├── auth/               # JWT
│   │   ├── security/           # RBAC, 限流, 审计, 密码哈希
│   │   ├── inference/          # llama.cpp 封装, KV Cache, 调度器
│   │   ├── rag/                # 分块, 嵌入, FAISS, BM25, 文档存储
│   │   └── service/            # ConfigManager
│   └── src/                    # 实现文件
│       └── main.cpp            # 入口 (路由注册)
├── web/                        # Next.js 前端
│   ├── next.config.ts          # API 代理配置
│   └── src/app/                # 页面
├── mcp-gateway/                # TypeScript Streamable HTTP MCP 网关
├── deploy/staging/             # Docker、部署脚本与真实服务 E2E
├── .github/workflows/          # CI 与 Staging 工作流
├── ARCHITECTURE.md             # 架构说明
├── scripts/                    # PDF 处理与工具脚本 (Python)
└── tests/                      # 测试
```

## 配置说明

`config.json` 结构 (模板见 `config.example.json`):

```
server.port          HTTP 端口 (默认 8080)
mysql.{host,port,user,password,database,pool_size}
jwt.{secret,expiry_seconds}
admin.{username,default_password}         首次启动创建的管理员
inference.{n_ctx,n_batch,n_threads,...}   LLM 推理参数
rag.storage_dir                           向量索引存储目录
rag.query_rewrite.{enabled,model,...}     将用户查询重写为英文检索 query, 失败自动回退
rate_limiter.rules[]                      聊天接口进程内固定窗口限流
scheduler.num_workers                     推理线程数
http_threads                              HTTP 线程数
remote_engine.{endpoint,model,api_key}    远程生成服务（本地模型未加载时也可使用）
```

命令行参数优先级: `--port/--model/... > config.json > 代码默认值`

远程模型的上下文容量由 `remote_engine.context_window_tokens` 显式配置，不会自动从模型名称推断。
使用支持百万上下文的 DeepSeek Flash 时，可设为 `1000000`，并将
`agent.max_output_tokens` 设为 `32768`，作为 Agent 聊天的默认回答上限；
`inference.max_tokens` 可同步设为 `32768`。本地模型仍需按实际部署容量配置。
聊天请求可通过 `max_tokens` 覆盖默认值，允许范围为 `1` 至 `65536`；
`agent.max_output_tokens = 0` 表示不向远程服务指定输出上限。
上下文检查保留 10% 余量，并扣除回答预留，剩余空间用于输入和工具交互。
部署此配置时需要包含支持上述输出范围的后端版本，并重新创建后端容器使配置生效。

## API 参考

### 认证
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/auth/login` | 登录, 返回 JWT token |

### 聊天
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/chat/stream` | SSE 流式对话 (支持 RAG 开关) |

### 会话
| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/sessions` | 会话列表 |
| POST | `/api/sessions` | 创建会话 |
| GET | `/api/sessions/:id` | 会话详情 (含消息) |
| DELETE | `/api/sessions/:id` | 删除会话 |
| GET | `/api/sessions/:id/messages` | 会话消息列表 |

### 文档 (RAG)
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/documents` | 上传文档, 返回 processing 后后台 chunk + embed |
| GET | `/api/documents` | 文档列表 (本组 + 公共, 含状态) |
| DELETE | `/api/documents/:id` | 删除文档 (admin, 软删除) |
| POST | `/api/documents/search` | RAG 检索 (混合:向量+BM25) |

### 租户管理 (admin only)
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/tenants` | 创建租户 |
| GET | `/api/tenants` | 租户列表 |

### 用户管理 (admin only)
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/users` | 创建用户 |
| GET | `/api/users` | 用户列表（含专属额度、有效默认额度、今日上限和用量） |
| POST | `/api/users/:id/quota` | 设置用户每日额度，同时更新今日上限，保留已用量 |
| DELETE | `/api/users/:id` | 删除用户 |

创建用户可选传入 `daily_token_quota`；修改额度请求示例：

```json
{"daily_token_quota": 2147483647}
```

`daily_token_quota: null` 恢复继承租户 `token_limit_daily`；指定额度必须为
1 至 9,007,199,254,740,991 的整数（JSON/JavaScript 可精确表示的最大整数）。
不提供此字段的创建请求默认继承；修改请求必须提供该字段。
admin 同样适用，可通过专属额度长期使用更高上限。额度不支持以 0 表示无限。
每日记录按 MySQL `CURDATE()` 创建，使用 `COALESCE(users.daily_token_quota, tenants.token_limit_daily)`；
用户额度修改会在同一事务中更新默认值和今日上限，历史记录与累计已用量不变。
当新上限低于已用量时，剩余额度为 0。修改租户默认值仍不会追溯修改已有每日记录。

已有数据库必须先执行 `sql/migrate_user_daily_token_quota.sql`，再启动新后端；
新数据库使用 `sql/init.sql`。迁移不改变已有额度和用量，也不会自动给 admin 添加覆盖值。
回滚应用时可以保留新增列及 BIGINT 字段，避免删除额度设置或用量数据。

### 登录续期与密码

- `POST /api/auth/login`：后端返回短期 JWT、一次性轮换刷新凭证和用户信息。
  浏览器经 Next.js 认证代理登录，刷新凭证只写入 HttpOnly、SameSite=Strict Cookie，不出现在浏览器响应 JSON 或 localStorage/sessionStorage。
  反向代理需将 /api/auth/* 交给前端认证路由，不能绕过该路由直接转发至 C++。
- `POST /api/auth/refresh`：通过刷新凭证续期，不要求旧 access token 仍有效；旧刷新凭证在轮换后不可重用。
  后端 API 客户端在 JSON 中传 refresh_token，网页由认证代理从 Cookie 读取。
- `POST /api/auth/logout`：撤销当前登录会话并清理 Cookie；有效 JWT 可按会话 ID 撤销，避免与刷新竞争。
- `GET /api/auth/me`：返回当前身份及 must_change_password。
- `POST /api/auth/password`：传 current_password、new_password；验证旧密码，更新哈希与凭证版本，撤销该用户所有会话，之后重新登录。
- `POST /api/users/:id/password`：管理员设置临时 password，撤销目标用户所有会话，不要求再次修改；不能用此接口重置自己。
- 创建和重置用户密码均不强制后续改密；must_change_password 保留为兼容字段，接口返回 false。
- 新密码为 8 至 16 个字符，允许弱密码，不限制字符组合，也允许重复使用原密码；游客不支持改密或续期，仍按游客有效期结束体验。

默认 JWT 有效期 900 秒，auth.refresh_expiry_seconds 为 2592000 秒（30 天，绝对到期，续期不无限延长）。
配置已有 jwt.expiry_seconds 时继续使用该值。所有刷新会话保存在 MySQL auth_sessions 中，仅保存刷新凭证的 SHA-256。
users.auth_version 在密码变更时递增。认证会检查数据库中的当前角色、组、状态、版本与登录会话，支持多后端实例。
没有新会话字段的旧 JWT 仅在用户凭证版本仍为 0 时兼容，密码变更后同样失效。
网页首次升级需重新登录一次建立 Cookie；此后支持刷新页面恢复、临近过期自动续期、401 重试，以及同页/双标签续期合并或串行化。返回前台会检查续期，退出事件按用户在标签页间同步；迟到响应不会恢复已退出的身份。

已有数据库先执行 sql/migrate_auth_sessions.sql，再启动新后端。迁移保留旧密码、角色、额度、会话及文档，现有用户不会被强制改密。
应用回滚可以保留新增列和表，不应恢复旧密码或删除会话/业务数据。回滚到旧认证代码会恢复旧的撤销语义，仅适合临时恢复服务。

### 对话 PDF 附件

对话输入框的 `+` 可上传 PDF，复用组库上传与 SHA-256 查重流程；重复文件直接复用已有文档。
上传仍沿用现有论文卡片生成流程；正文缓存与卡片分开保存。

- `POST /api/chat/attachments`：`{"doc_id": 123}`，校验文档权限、准备正文缓存，返回文件名、页数、哈希和版本，不返回正文。
- `POST /api/chat/stream`：支持 `attachments: [123]`，每条消息最多 4 个 PDF；无问题文本时默认分析附件。
- 模型接收完整逐页正文；历史附件在后续问题中重新校验权限并加载，每个文件哈希只加入一次。
- 用户消息的 `messages.rag_references` 记录附件文档 ID、文件名、哈希和提取版本；会话接口以 `attachments` 返回。
  用户问题单独保存，不将正文写入数据库；助手消息继续使用该列保存检索来源。无需新增数据库迁移。
- 正文位于 `processed/pdf-text/<sha256>.poppler-layout-v1.pages.json`，文件锁避免重复提取，原子替换避免半成品。
  缓存损坏可从原 PDF 重建；版本变更使用新文件。重命名不影响哈希缓存。
- 后端配置 `knowledge_fetch.text_cache_directory`，默认取 PDF 目录同级的 `processed/pdf-text`。
  前端使用 `PDF_PROCESSED_DIR/pdf-text`；二者需挂载同一持久化目录并具有共享读写权限。
  原 PDF、正文缓存与数据库一起备份。多机部署需使用共享存储。
- 上传最大 30 MiB；单文件最多 1000 页、正文最多 4 MiB；会话 PDF 上下文最多 16 份、合计 4 MiB。
  超限或模型上下文不足会明确报错，不静默裁剪正文。扫描件需先做 OCR，当前不自动 OCR。
- 删除或失去权限的附件不会继续发送给模型；已有会话会报错，可新开会话并附加可用文件。
- 前后端均需要 Poppler；PDF 准备在有界工作线程中执行。

### 健康检查
| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 服务状态 (MySQL/模型) |

## 数据库设计 (9 张表)

```
tenants       — 租户 (id, name, token_limit_daily, status)
users         — 用户 (id, tenant_id, username, password_hash, daily_token_quota, role, status)
sessions      — 对话会话 (id, tenant_id, user_id, title, model)
messages      — 消息记录 (id, session_id, role, content, tokens_in/out, rag_references)
documents     — 文档 (id, tenant_id, filename, content, chunk_count, visibility, status)
document_chunks — 文档分块 (id, document_id, tenant_id, chunk_index, content, visibility)
audit_logs    — 审计日志 (tenant_id, user_id, action, resource, result)
user_token_usage_daily — 用户每日 Token 用量 (tenant_id, user_id, usage_date, token_limit, tokens_used)
token_usage_records — 请求级 Token 使用记录 (request_id, session_id, prompt/completion/total_tokens, status)
```

- 文档状态: documents.status = 'processing' | 'ready' | 'failed' | 'deleted'
- 文档检索按经过认证的 tenant_id 与 visibility 过滤；会话消息通过所属会话关联租户。
- chunk_id (MySQL auto_increment) 同时作为 FAISS entry_id, 无需额外映射
