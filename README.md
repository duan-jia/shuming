<p align="center">
  <img src="web/public/shuming-icon.png" width="96" height="96" alt="shuming 项目图标" />
</p>

# shuming · 枢明

面向实验室的知识库与研究助手：管理论文和文档，通过 RAG 检索、Agent 工具和流式对话使用知识。EMOS 是访问 shuming 的聊天前端。

**一个仓库，三个边界清楚的服务：知识后端、聊天前端、生成模型。** 多租户、个人与团队知识/记忆库的长期设想不等于已实现的功能。

[快速开始](#快速开始) · [模型部署](deploy/models/README.md) · [开发指南](docs/getting-started.md) · [配置与API](docs/reference.md) · [架构](ARCHITECTURE.md)

## 当前能力

- 文档与论文：上传、处理状态、论文卡片、分块及课题组/公共可见性；暂不支持扫描PDF的OCR。
- 检索：本地 BGE 嵌入、FAISS + BM25 混合检索，复用统一知识检索服务。
- 问答：SSE 流式会话，Agent 调用知识库/网页的搜索和读取工具，支持取消、预算与权限控制。
- 接入：REST 与 C++ 原生 MCP；JWT/RBAC、租户隔离、限流。网页搜索是独立可选服务。

## 架构与默认模型

```text
浏览器 → EMOS / Next.js → shuming / C++ → OpenAI兼容生成API
                              ↓                ↓
                    BGE + FAISS + BM25    Qwen3.8-27B
                              ↕          独立llama-server
                    MySQL / 本地索引     单张3080 + CPU卸载
Agent → /mcp/knowledge、/mcp/web、/mcp
```

| 项目 | 默认/说明 |
|---|---|
| 生成模型 | **Qwen3.8-27B**，API别名同名 |
| 模型服务 | 本机独立 llama-server，`http://127.0.0.1:8010/v1` |
| 3080启动配置 | Q4_K_M、部分GPU层、单并发、8192上下文；**尚未GPU实测** |
| 嵌入 | BGE-small-zh-v1.5 GGUF，512维，不随生成模型更换 |
| 后端 / 前端 | C++17 / Next.js；原始llama.cpp内嵌路径保留兼容 |

**27B并不意味着能完整装进3080。** 即使按4-bit计算，纯权重理论下限也约13.5GB，尚未计入量化元数据、运行缓冲和KV。本方案以CPU/主机内存卸载为前提，不承诺吞吐、显存占用或完整Agent/视觉兼容性。模型部署与知识数据库部署互相独立。

## 快速开始

本次仅调整接口、默认配置与文档；以下为操作者后续执行步骤，不代表已经部署。

1. 获取源码，按[开发指南](docs/getting-started.md)安装依赖、准备MySQL与BGE嵌入模型。
2. 按[模型指南](deploy/models/README.md)准备量化权重和独立llama-server。先查看命令，不启动服务：

   ```bash
   node deploy/models/local-model.mjs plan
   ```

3. 创建配置，填写MySQL、随机JWT密钥和管理员密码；不要使用示例凭据对外服务：

   ```bash
   cp config.example.json config.json
   chmod 600 config.json
   # 编辑 config.json 的数据库、JWT、管理员设置
   # local-no-auth只是本机无鉴权模型服务的占位值，不是真实API密钥。
   ```

4. 构建并启动后端。默认生成走模型HTTP接口，不需要传27B的 `--model`：

   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF
   cmake --build build --target fast-llm-reasoner --parallel 2
   ./build/bin/fast-llm-reasoner --config config.json --port 9006 \
     --embedding-model models/bge-small-zh-v1.5-q4_k_m.gguf
   cd web && npm ci && npm run dev
   ```

   前端默认访问localhost:3000，API代理指向127.0.0.1:9006。跨主机/容器需另行配置服务地址；容器里的127.0.0.1不是宿主机。不要把模型未鉴权端口公开。

## fast-llm-reasoner 还保留什么？

**不再作为产品名或仓库名；仅作为历史兼容标识保留。** 当前可执行文件/CMake target、部分镜像和部署资源仍使用旧名，所以启动命令中的旧名不是另一个项目。

本次不批量改二进制、C++命名空间、现有Compose项目、数据库名或数据卷，也不会为改名重建资源。尤其不要删除 `fast-llm-reasoner-staging_mysql-data` 等已有数据卷。后续若统一技术标识，应另做有迁移与回滚方案的变更。

README顶部复用已有项目图标；它不改变GitHub账号头像、仓库可见性或Social preview设置。

## 文档导航

| 目标 | 文档 |
|---|---|
| 单张3080的模型服务与接口 | [Qwen3.8-27B配置](deploy/models/README.md) |
| 源码开发、PDF处理与基础验证 | [开发指南](docs/getting-started.md) |
| 数据结构、配置、API | [参考说明](docs/reference.md) |
| MCP接入与工具读取 | [原生MCP](docs/native-mcp.md) · [Agent Runtime](docs/agent-runtime.md) |
| 已有容器流程（不自动迁移） | [本地Docker](deploy/local/README.md) · [服务器](deploy/server/README.md) |
| CI/Staging（不含完整模型服务） | [Staging](deploy/staging/README.md) |
| Agent / RAG 评测与结果 | [评测指南](tests/README.md) · [评测报告](tests/reports/README.md) |
| 开发与评测环境隔离 | [共享开发评测环境](deploy/dev-eval/README.md) |
| 设计与开发约定 | [架构](ARCHITECTURE.md) · [AGENTS](AGENTS.md) |

## 配置说明

常用字段见[配置参考](docs/reference.md#配置说明)和[模板](config.example.json)。生成服务使用 `remote_engine`；“remote”表示进程外API，不意味着必须上云。通用环境变量 `REMOTE_ENGINE_ENDPOINT / MODEL / API_KEY` 可覆盖配置；旧 `DEEPSEEK_API_KEY` 仅保留兼容回退。

## API 参考

接口列表见[API参考](docs/reference.md#api-参考)。本次未更改对外聊天/MCP协议，也未升级后端内嵌llama.cpp依赖。

## 测试与验证

按本次需求，只做配置/启动参数的离线冒烟及静态检查。不运行大模型下载、3080推理、性能调参、完整后端编译或E2E；详细验证说明和后续命令见[模型指南](deploy/models/README.md)与[开发指南](docs/getting-started.md#测试与验证)。
