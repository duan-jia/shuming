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
| GET | `/api/users` | 用户列表 |
| DELETE | `/api/users/:id` | 删除用户 |

### 健康检查
| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 服务状态 (MySQL/模型) |

## 数据库设计 (9 张表)

```
tenants       — 租户 (id, name, token_limit_daily, status)
users         — 用户 (id, tenant_id, username, password_hash, role, status)
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
