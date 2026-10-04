# fast-llm-reasoner

实验室内部文档检索问答系统，基于本地 LLM + RAG（向量+关键词混合检索），支持多课题组数据隔离、SSE 流式对话。

本系统为实验室内部使用场景设计，旨在为课题组提供私有的文档检索和智能问答能力。支持多课题组（租户）之间的数据隔离，同时提供公共知识库供全实验室共享。单机部署，无需额外的容器编排或微服务组件。

## 技术栈

| 层次 | 技术 |
|---|---|
| 后端 | C++17, muduo (HTTP/SSE), CMake |
| LLM 推理 | llama.cpp + GGML CUDA |
| 主模型 | Qwen2.5-3B-Instruct Q4_K_M (~2.0GB) |
| 嵌入模型 | bge-small-zh-v1.5 Q4_K_M (~15MB, 512-dim) |
| 向量检索 | FAISS IndexFlatIP (进程内, 无额外服务) |
| 关键词检索 | BM25 (CJK unigram+bigram) |
| 存储 | MySQL 8.x (主存储) |
| 安全 | JWT + RBAC + 租户隔离 + 限流 + PBKDF2 密码哈希 |
| 前端 | Next.js (React/TypeScript) |

## 硬件要求

- **最低**: 无 GPU, CPU 推理 (慢)
- **推荐**: NVIDIA GPU, VRAM ≥ 6GB, CUDA ≥ 11.7
- **开发用**: 任意机器 (不编译 LLM 推理部分)

CUDA 架构号对照:

| GPU | CUDA 架构 | CMake 参数 |
|---|---|---|
| RTX 2060 | SM 75 | `-DCMAKE_CUDA_ARCHITECTURES=75` |
| RTX 3060 | SM 86 | `-DCMAKE_CUDA_ARCHITECTURES=86` |
| RTX 4090 | SM 89 | `-DCMAKE_CUDA_ARCHITECTURES=89` |
| 无 GPU | — | `-DGGML_CUDA=OFF` |

## 快速开始

### 1. 系统依赖

```bash
# Ubuntu/Debian
sudo apt install -y build-essential cmake git \
  libssl-dev libmysqlclient-dev \
  libopenblas-dev gfortran \
  mysql-server \
  nodejs npm

# CUDA Toolkit (有 GPU 时需要, 无 GPU 跳过)
sudo apt install -y nvidia-cuda-toolkit
```

### 2. 下载模型文件

将 GGUF 模型放入 `models/` 目录 (2GB, 不进 git):

```bash
# 主模型 (Qwen2.5-3B Q4_K_M)
# 下载: https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF
# 文件名: qwen2.5-3b-instruct-q4_k_m.gguf (约2.0GB)

# 嵌入模型 (BGE-small-zh Q4_K_M)
# 下载: https://huggingface.co/ChristianAzinn/bge-small-zh-v1.5-gguf
# 文件名: bge-small-zh-v1.5-q4_k_m.gguf (约15MB)
```

### 3. 配置

```bash
cp config.example.json config.json
vim config.json   # 修改 MySQL 密码、JWT secret、admin 默认密码
```

### 4. 初始化数据库

```bash
sudo systemctl start mysql
mysql -u root < sql/init.sql
```

### 5. 编译

```bash
# 有 GPU
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=75 -DGGML_CUDA=ON

# 无 GPU (CPU only)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF

cmake --build build --target fast-llm-reasoner -j$(nproc)
```

### 6. 运行

```bash
# 仅嵌入模型 (RAG 可用, 聊天不可用)
./build/bin/fast-llm-reasoner --port 9006 \
  --embedding-model models/bge-small-zh-v1.5-q4_k_m.gguf

# 完整启动 (LLM + RAG)
./build/bin/fast-llm-reasoner --port 9006 \
  --model models/qwen2.5-3b-instruct-q4_k_m.gguf \
  --embedding-model models/bge-small-zh-v1.5-q4_k_m.gguf

# 自定义配置文件
./build/bin/fast-llm-reasoner --config /path/to/other-config.json

# 前端开发
cd web && npm install && npm run dev
# 访问 http://localhost:3000
```

### 7. 首次登录

服务器首次启动自动创建管理员:

```
用户名: admin
密码:   changeme123  (config.json 中 admin.default_password 可改)
```

**生产环境请立即修改密码。**

## 跨机器开发工作流

```
┌──────────────┐    git push    ┌──────────┐    git pull    ┌──────────────┐
│  开发机       │ ────────────▶ │  GitHub  │ ────────────▶ │  WSL2 本地机  │
│  只写代码     │               │          │               │  编译 + 运行   │
│  前端 dev     │ ◀──────────── │          │ ◀──────────── │  (有 GPU)     │
└──────────────┘    git pull    └──────────┘    git push    └──────────────┘
```

- **开发机**: 拉代码, `cd web && npm run dev` 开发前端, 改 C++ 后 push
- **部署机 (WSL2)**: `git pull`, `cmake --build build`, 启动服务器
- 模型文件不在 git 里, 两边各自下载/拷贝放到 `models/`
- 配置各自一份 `config.json` (不进 git), 改 MySQL 密码等

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
remote_engine.{endpoint,model,api_key}    DeepSeek 作为 fallback
```

命令行参数优先级: `--port/--model/... > config.json > 代码默认值`

## API 端点 (17个)

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

## MCP 接入

`mcp-gateway/` 提供面向内部 Agent 的远程 Streamable HTTP MCP 网关。首版工具
`search_knowledge` 复用上述 RAG 检索接口，租户由经过认证的 service principal 绑定，
而不是由工具参数传入。配置、运行和验证方法见 [`mcp-gateway/README.md`](mcp-gateway/README.md)。

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

安装唯一的文档处理依赖：

```bash
python3 -m pip install pypdf
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
PROJECT_ROOT=/path/to/fast-llm-reasoner
PDF_PROCESSOR_PYTHON=/path/to/python3
PDF_SOURCE_DIR=/path/to/data/pdfs
PDF_PROCESSED_DIR=/path/to/data/processed
BACKEND_API_BASE=http://127.0.0.1:9006
```

在线检索仍会使用配置的 query rewrite；失败时自动回退到原始 query。

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

## 项目结构

```
fast-llm-reasoner/
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
├── scripts/                    # 评估脚本 (Python)
└── tests/                      # 测试
```

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
- 多租户隔离: 所有表含 tenant_id, 文档搜索过滤 tenant_id + visibility
- chunk_id (MySQL auto_increment) 同时作为 FAISS entry_id, 无需额外映射
