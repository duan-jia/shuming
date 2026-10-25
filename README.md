# shuming · 枢明

shuming（枢明）是实验室的后端知识库，提供文档管理、RAG 检索、模型推理与 MCP 接口。面向 EMOS 使用的聊天前端名为 **EMOS**，通过 API 访问 shuming。

系统面向课题组私有资料与实验室公共知识，支持多租户隔离和 SSE 流式问答。当前保持单仓库、共享检索核心的结构；个人与团队的知识/记忆库是远期方向，不代表这些能力已经实现。

> 仓库已从 `fast-llm-reasoner` 改名为 `shuming`。本次只更新文档与品牌名称，CMake target、可执行文件、npm 包名、数据库及部署资源名继续保留旧名称，以避免破坏兼容性。

## 当前能力

- **知识检索**：BGE 本地嵌入、FAISS 向量检索与 BM25 关键词检索。
- **问答与会话**：远程模型 Agent Loop 自主调用知识库/网页工具，支持 SSE 状态、取消和预算；本地 llama.cpp 保留原有生成路径。
- **知识管理**：文档导入、处理状态、删除，以及课题组/公共文档可见性。
- **访问控制**：JWT、RBAC、租户隔离、限流与密码哈希。
- **Agent 接入**：C++ 原生 MCP 提供 `search_knowledge` 与可选 `search_web`，复用后端身份与租户权限。
- **论文处理**：文本型 PDF 抽取、远程模型生成论文卡片、入库检索；暂不支持扫描 PDF/OCR。

## 当前架构

```text
用户 → Next.js 前端 → C++ 后端 → 本地 / 远程生成模型
                          ↓
                   KnowledgeSearchService
                          ↓
                 RAG：BGE + FAISS + BM25
                          ↕
                    MySQL / 本地索引
                          ↑
Agent → C++ /mcp/knowledge → KnowledgeSearchService
Agent → C++ /mcp/web → DeepSeek Search API（可选）
```

问答、REST 检索和原生 MCP 共用后端知识检索能力；两个 MCP 入口运行在同一 C++ 进程中，工具业务层独立。原 TypeScript 网关保留供旧部署兼容。详细说明见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 技术栈与运行模式

| 层次 | 技术 |
|---|---|
| 前端 | Next.js / React / TypeScript |
| 后端 | C++17 / muduo / HTTP / SSE / CMake |
| 生成模型 | llama.cpp（本地 GGUF）或 remote_engine（远程 API） |
| 嵌入模型 | bge-small-zh-v1.5 GGUF，512 维 |
| 检索 | FAISS IndexFlatIP + BM25（CJK unigram/bigram） |
| 存储 | MySQL 8.x + 本地检索索引 |
| MCP | C++ JSON-RPC / Streamable HTTP；TypeScript 兼容网关 |
| 部署与检查 | Docker Compose / GitHub Actions |

| 模式 | 需要的模型与服务 | GPU |
|---|---|---|
| 仅知识检索 / MCP | MySQL + 本地嵌入模型 | 不要求 |
| 远程模型问答 | 上述组件 + 可用的远程生成 API | 不要求 |
| 本地模型问答 | 上述组件 + 本地主模型 | 可 CPU 运行；GPU 可加速 |

实际内存与性能取决于模型、文档规模和并发量。Staging 仅验证知识检索/MCP，不包含前端和本地主模型，不能据此认定完整问答部署已验收。

## 文档导航

- [本地 Docker 测试](deploy/local/README.md)
- [本地快速开始](#快速开始)
- [Agent Runtime](docs/agent-runtime.md)
- [原生 MCP 配置与使用](docs/native-mcp.md) · [旧 TS 网关](mcp-gateway/README.md)
- [Staging 部署](deploy/staging/README.md)
- [架构说明](ARCHITECTURE.md)
- [开发约定](AGENTS.md)
- [配置说明](#配置说明) · [API 参考](#api-参考)


## 快速开始

Docker 服务器部署见[服务器部署指南](deploy/server/README.md)。

### 1. 获取源码与安装依赖

```bash
git clone https://github.com/duan-jia/shuming.git
cd shuming

# Ubuntu/Debian
sudo apt install -y build-essential cmake git \
  libssl-dev default-libmysqlclient-dev libboost-all-dev \
  libopenblas-dev gfortran \
  mysql-server \
  python3 python3-pip python3-venv

# Node.js 24 与 npm 请单独安装；MCP CI 使用 Node.js 24。
# 验证：node --version && npm --version

# CUDA Toolkit (有 GPU 时需要, 无 GPU 跳过)
sudo apt install -y nvidia-cuda-toolkit
```

### 2. 下载模型文件

将 GGUF 模型放入 `models/`（不进 Git）。检索需要嵌入模型；只有本地生成回答才需要约 2GB 的主模型。

```bash
mkdir -p models
```

```bash
# 主模型 (Qwen2.5-3B Q4_K_M)
# 下载: https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF
# 文件名: qwen2.5-3b-instruct-q4_k_m.gguf (约2.0GB)

# 嵌入模型 (BGE-small-zh Q4_K_M)
# 下载: https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf
# 文件名: bge-small-zh-v1.5-q4_k_m.gguf (约15MB)
```

### 3. 配置

```bash
cp config.example.json config.json
vim config.json   # 设置 MySQL、JWT secret、管理员密码；远程问答还需配置 remote_engine
```

不要使用模板中的默认密码或 JWT secret 对外提供服务。数据库默认名仍为 `llm_platform`。

### 4. 初始化数据库

```bash
sudo systemctl start mysql
sudo mysql < sql/init.sql
# 如数据库使用密码认证，可改用 mysql -u root -p < sql/init.sql
```

### 5. 编译

```bash
# 有 GPU
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=75 -DGGML_CUDA=ON

# 无 GPU (CPU only)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF

cmake --build build --target fast-llm-reasoner --parallel 2
```

### 6. 运行

```bash
# 远程问答 + 本地 Embedding：先配置 remote_engine
# 未配置可用的远程生成服务时，此模式仅用于检索
./build/bin/fast-llm-reasoner --port 9006 \
  --embedding-model models/bge-small-zh-v1.5-q4_k_m.gguf

# 本地问答 + RAG（需要本地主模型）
./build/bin/fast-llm-reasoner --port 9006 \
  --model models/qwen2.5-3b-instruct-q4_k_m.gguf \
  --embedding-model models/bge-small-zh-v1.5-q4_k_m.gguf

# 自定义配置文件
./build/bin/fast-llm-reasoner --config /path/to/other-config.json

# 前端开发
cd web && npm ci && npm run dev
# 访问 http://localhost:3000
```

前端 API 代理默认指向 `127.0.0.1:9006`，因此上述示例显式使用 `--port 9006`；
配置文件默认端口为 8080。跨主机或 Docker 部署时，在构建前设置 `BACKEND_API_BASE`，
并在运行时设置相同值，以同时配置通用 API 代理和服务端文档路由。

### 7. 首次登录

服务器首次启动自动创建管理员:

```
用户名: admin
密码:   changeme123  (config.json 中 admin.default_password 可改)
```

**生产环境请立即修改密码。**

## MCP 接入

C++ 后端直接提供 Streamable HTTP MCP：`/mcp/knowledge` 暴露 `search_knowledge`，
`/mcp/web` 暴露可选的 `search_web`，`/mcp` 提供全部已获授权工具。共用现有 JWT，
知识库租户取自认证身份，检索直接调用共享服务；网页搜索通过 DeepSeek Search API。
配置与验证见 [原生 MCP](docs/native-mcp.md)。

`mcp-gateway/` 作为旧 TS 转发网关保留，现有部署可继续使用；新入口无需启动它。

## PDF 文档上传与 RAG

文档链路只支持带可提取文本的 PDF：

```text
data/pdfs/<name>.pdf
        ↓ scripts/pdf_to_markdown.py
data/processed/<name>.md + <name>.json
        ↓ POST /api/documents
data/rag/ (FAISS + BM25)
```

聊天页面的“上传 PDF”会保存源文件、调用远程大模型生成紧凑论文卡片，再把 Markdown
作为当前 tenant 的 `group` 文档提交给 RAG。Markdown 包含 Problem and Context、Core Idea
and Approach、Key Takeaways、Scope and Limitations；JSON 保存相同内容及来源哈希、模型和
生成信息。论文卡片规则集中在 `scripts/paper_card.py`，扫描版 PDF 暂不支持。

文档处理使用 Python 与 `pypdf`。建议通过虚拟环境安装（Ubuntu 24.04 不宜直接向系统 Python 安装）：

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install pypdf
```

命令行可以先检查文本抽取，或生成实际结果：

```bash
python3 scripts/pdf_to_markdown.py --file data/pdfs/example.pdf --dry-run
python3 scripts/pdf_to_markdown.py --file data/pdfs/example.pdf
```

脚本默认读取 `config.json` 的 `remote_engine.endpoint`、`model` 和 `api_key`。可用
`REMOTE_ENGINE_ENDPOINT`、`REMOTE_ENGINE_MODEL`、`REMOTE_ENGINE_API_KEY` 覆盖；API key
也兼容 `DEEPSEEK_API_KEY`。

Next.js 上传服务支持以下部署变量：

```bash
PROJECT_ROOT=/path/to/shuming
PDF_PROCESSOR_PYTHON=/path/to/python3
PDF_SOURCE_DIR=/path/to/data/pdfs
PDF_PROCESSED_DIR=/path/to/data/processed
BACKEND_API_BASE=http://127.0.0.1:9006
```

在线检索仍会使用配置的 query rewrite；失败时自动回退到原始 query。

## 部署与开发流程

本地开发可以运行 MySQL、后端和前端；模型、数据与配置均不提交到 Git。CPU 构建使用 `-DGGML_CUDA=OFF`；GPU 构建按实际硬件设置 CUDA 架构，示例中的 75 对应 RTX 2060，86 对应 RTX 3060，89 对应 RTX 4090。

Staging 使用 [Docker Compose](deploy/staging/compose.yml) 部署 MySQL、后端和 MCP。GitHub-hosted runner 构建镜像并嵌入已校验的 BGE 模型，服务器拉取镜像后运行真实服务端到端测试。具体见 [部署说明](deploy/staging/README.md) 与 [工作流](.github/workflows/staging-e2e.yml)。

- Staging 不部署 Web 前端与本地主模型，也不是完整生产部署方案。
- MySQL 位于 Compose 内部网络，后端与 MCP 仅绑定宿主机回环地址。
- 对外访问需额外配置 HTTPS、认证、网络访问控制和备份。
- 镜像、Compose 项目与服务器目录仍保留 `fast-llm-reasoner` 技术名称；不要仅为改名而重建数据卷。

日常变更通过开发分支与 PR 集成，检查通过后 squash merge；不直接推送 `master`。仓库改名后，已有本地副本可执行：

```bash
git remote set-url origin https://github.com/duan-jia/shuming.git
```

## 测试与验证

后端（已配置 CPU/GPU 构建目录）：

```bash
cmake --build build --target knowledge-search-service-test fast-llm-reasoner fast-llm-cli --parallel 2
./build/bin/knowledge-search-service-test
```

MCP 网关：

```bash
cd mcp-gateway
npm ci
npm run typecheck
npm test
npm run build
npm audit --omit=dev --audit-level=high
```

前端（单独在仓库根目录开始）：

```bash
cd web
npm ci
npm run lint
npm run build
```

评测分为两个独立维度，入口、指标及标注规则见 [tests/README.md](tests/README.md)：

```bash
# 当前论文集：30 篇、100 条 RAG 查询、30 个 Agent 场景
node --test tests/datasets/recent-papers-v1/tests/dataset.test.mjs
python3 -m unittest discover -s tests -p 'test_evaluation.py'
# 对含参数与结果的 Agent 归一化轨迹评分
python3 tests/agent_eval.py --predictions /path/to/agent-traces.jsonl
# 设置 EMOS_EVAL_TOKEN，并使用语料导入映射采集真实检索结果
python3 tests/rag_eval.py --base-url http://127.0.0.1:9007 --document-map /path/to/paper-doc-map.json --k 1 3 5 10
# 论文卡片单元测试
python3 -m unittest discover -s tests -p 'test_paper_card.py'
```

[CI](.github/workflows/ci.yml) 执行指标回归、后端单元测试、受控 MCP/Agent 集成测试、部署隔离检查以及前端检查。真实模型的工具选择与真实知识库的检索质量需要单独运行上述评测，不以受控夹具通过代替质量结论。


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
