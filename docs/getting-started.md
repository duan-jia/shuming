# 开发、PDF处理与验证

[返回首页](../README.md) · [配置/API参考](reference.md)

下面的构建和数据库说明用于实际开发；本次品牌/模型接口调整未执行这些完整部署和细节测试。

## 快速开始

默认生成模型部署见[Qwen3.8-27B / RTX 3080](../deploy/models/README.md)。旧服务器方案见[服务器部署指南](../deploy/server/README.md)。

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

### 2. 准备嵌入模型与生成服务

检索仍使用BGE-small-zh-v1.5 GGUF（512维），放入models/，不进Git。生成服务默认采用独立进程的Qwen3.8-27B，参考[部署配置](../deploy/models/README.md)。不要把27B主模型作为BGE嵌入替代，也不要把它直接传给本仓库旧内嵌模型路径。

```bash
mkdir -p models
# BGE来源（不是27B生成模型）：
# https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf
# 文件：bge-small-zh-v1.5-q4_k_m.gguf
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
  -DCMAKE_CUDA_ARCHITECTURES=86 -DGGML_CUDA=ON

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

# 兼容旧内嵌GGUF路径时，可另行使用 --model；它不是默认27B服务。
# 不要求为外部模型给本后端启用CUDA；BGE可使用CPU构建。

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

C++ 后端直接提供 Streamable HTTP MCP：`/mcp/knowledge` 暴露 `search_knowledge` / `fetch_knowledge`，
`/mcp/web` 暴露可选的 `search_web` / `fetch_web`，`/mcp` 提供全部已获授权工具。共用现有 JWT，
知识库租户取自认证身份，检索直接调用共享服务；网页搜索通过 DeepSeek Search API。
配置与验证见 [原生 MCP](native-mcp.md)。

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

本地开发可以运行 MySQL、后端和前端；模型、数据与配置均不提交到 Git。CPU 构建使用 `-DGGML_CUDA=OFF`；GPU 构建按实际硬件设置 CUDA 架构；RTX 3080使用86。外部生成服务与后端独立编译，内嵌路径仅保留兼容。

Staging 使用 [Docker Compose](../deploy/staging/compose.json) 部署 MySQL、后端和 MCP。GitHub-hosted runner 构建镜像并嵌入已校验的 BGE 模型，服务器拉取镜像后运行真实服务端到端测试。具体见 [部署说明](../deploy/staging/README.md) 与 [工作流](../.github/workflows/staging-e2e.yml)。

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

论文卡片单元测试与检索质量评测：

```bash
python3 -m unittest discover -s tests -p 'test_paper_card.py'
python3 -m unittest discover -s tests -p 'test_evaluation.py'
python3 -m unittest discover -s tests -p 'test_agent_model_eval.py'
node tests/datasets/recent-papers-v1/scripts/validate.mjs
node --test tests/datasets/recent-papers-v1/tests/dataset.test.mjs
# 需要已导入的知识库、文档映射与 EMOS_EVAL_TOKEN
python3 tests/rag_eval.py --base-url http://127.0.0.1:9007 --document-map /path/to/paper-doc-map.json --k 1 3 5 10
```

Agent 工具契约、RAG 指标和数据集限制见 [评测指南](../tests/README.md)。

[CI](../.github/workflows/ci.yml) 检查后端 CPU 构建/单元测试与 MCP 类型、测试、构建、安全审计。Staging E2E 另外验证真实 MySQL/后端/MCP 的服务账号认证、租户绑定与检索；它不等价于检索质量评测，也不覆盖完整 Web 问答链路。


