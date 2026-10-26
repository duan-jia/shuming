# 项目架构说明

## 1. 系统定位

`fast-llm-reasoner` 是面向实验室内部场景的本地 LLM + RAG 问答系统。核心目标不是做复杂的多租户 SaaS，而是为多个课题组提供可控的文档检索、对话问答、权限隔离和基础用量限制。

系统当前采用单体后端架构：C++ 后端负责 HTTP API、认证鉴权、会话管理、RAG 检索、LLM 推理调度和 MySQL 持久化；Next.js 前端负责交互界面；MySQL 是主存储；FAISS 与 BM25 在后端进程内维护检索索引。

## 2. 总体分层

后端代码主要位于 `engine/`：

```text
engine/include/     公共头文件
engine/src/         C++ 实现
engine/src/main.cpp 应用入口与路由注册
```

核心分层如下：

```text
routes      HTTP API 入口，解析请求并调用 service
services    业务编排与共享工具层，处理认证、对话、文档、检索等业务流程
agent       模型与工具调用循环、预算和停止控制
mcp         对外 Streamable HTTP / JSON-RPC 工具适配
persistence MySQL 访问层，封装 Repository 与事务
rag         文档分块、查询重写、向量索引、BM25、权限过滤与召回
inference   本地模型、远程模型、批处理调度与推理抽象
security    JWT、RBAC、租户隔离、限流、审计日志
server      HTTP server、router、SSE 连接管理
app         AppContext 与 AppBootstrap，负责启动期依赖装配
```

`main.cpp` 只保留启动编排：读取配置、初始化 `AppContext`、创建 service、注册 routes、启动 HTTP server。具体业务逻辑应继续下沉到 service/repository/rag/inference 层。

## 3. 核心数据模型

数据库初始化脚本在 `sql/init.sql`。当前主要表包括：

- `tenants`：实验室组/课题组，保存组级默认每日 token 限额。
- `users`：用户，属于某个 tenant，角色为 `admin`、`user` 或 `guest`。
- `sessions`：一次对话会话。
- `messages`：会话消息，保存 user/assistant 内容、reasoning、RAG 引用和 token 统计。
- `documents`：导入后的文档元信息与内容，当前 visibility 使用 `public` / `group`，其中 `group` 等价于业务语义中的 tenant 可见。
- `document_chunks`：文档切块后的检索单元。
- `user_token_usage_daily`：用户每日 token 用量聚合。
- `token_usage_records`：每次请求的 token 使用明细，使用 `request_id` 保证幂等。
- `audit_logs`：关键操作审计。

文档权限模型应保持简单：

```text
public：全实验室已登录用户可读，仅 admin 上传和删除
tenant/group：本研究组成员可读、上传及删除（包括他人上传的组内文件）
```

当前不设计 private 个人知识库。每个用户属于一个研究组；guest 统一归属测试实验组，令牌最长有效 15 分钟。公开文档保留原研究组与上传者信息，但写入权限由公开库规则决定。

权限实现与角色矩阵见 [权限管理](docs/permissions.md)。受保护的 REST、聊天和 MCP 共用认证与接口权限策略，数据查询继续执行租户和所有者过滤。

## 4. 对话请求流程

典型对话请求由 `/api/chat/stream` 进入：

```text
前端 SSE 请求 → JWT / 限流 / session 所有权 / 历史与额度检查
↓
有界推理队列（同一 session 同时仅一个请求）
↓
远程模型：Agent Runtime → 模型返回文本或 tool_calls
                           ↓ tools/call
                     进程内 ToolService
                     ├─ KnowledgeSearchService
                     └─ WebSearchService
                           ↓ 工具结果加入当前上下文
                         再次调用模型
↓
流式返回工具状态、reasoning / answer、done 或 error
↓
汇总所有模型轮次用量，结算并保存已生成的 user/assistant 对话
```

`agent.enabled=false` 或使用不支持工具的本地模型时，继续原有单次生成路径。
取消通过 `/api/chat/cancel` 或 SSE 断线触发；网络请求支持中断，RAG 在同步计算边界
检查取消，不能保证立即打断本地嵌入计算。

当前系统规模约 500 人，优先使用 MySQL 同步落库即可。这样事务边界清晰，排障简单，也避免 Redis 扣费与异步入库带来的补偿复杂度。

## 5. Token 计费与幂等

token 限额以用户为主，租户表中的 `token_limit_daily` 表示该 tenant 下用户的默认每日额度。

计费相关责任划分：

- `messages`：记录对话内容及本次消息的 token 统计。
- `token_usage_records`：记录一次请求的计费明细和状态。
- `user_token_usage_daily`：记录用户当天累计使用量，用于快速判断是否超限。

幂等通过 `token_usage_records.request_id` 唯一约束保证。同一请求重复提交时，不应重复扣减。扣费、记录明细和消息落库应尽量放在同一事务或明确的顺序控制中，保证“有记录可追踪、失败可恢复”。

## 6. RAG 文档导入流程

文档入口只支持 PDF。系统保留源 PDF 和最终文档卡片，不保存全文转换稿、治理清单或
批量导入 manifest：

```text
用户上传 PDF → data/pdfs
↓
scripts/pdf_to_markdown.py 抽取文本并调用远程大模型
↓
data/processed 中生成同名 Markdown 和 JSON
↓
Markdown 提交 POST /api/documents，visibility=group
↓
写入 documents/document_chunks，更新 data/rag 中的 FAISS/BM25
```

Markdown 用于 RAG，JSON 用于记录来源哈希、生成模型和结构化摘要。无法提取文本的扫描版
PDF 会停止在压缩阶段，不进入 RAG。

## 7. 检索架构

RAG 检索由 `engine/src/rag/` 负责，采用混合检索：

- QueryRewriter：将用户中文或混合语言 query 重写为英文检索 query，并保留原始 query；失败时回退原始 query。
- FAISS：对 card/chunk 内容做向量召回。
- BM25：对标题、英文关键词、缩写和正文摘要做词面召回。
- 权限过滤：检索时只返回 `public` 文档或当前用户 tenant 下的 `group` 文档。
- DocumentStore：统一管理文档、chunk、向量索引和 BM25 索引。

RAG 只导入压缩后的 Markdown；标题、关键词、Summary、Key Takeaways 和 Scope and Positioning
都会参与向量检索和 BM25 检索。精确数字、公式和原文措辞仍应回到源 PDF 核验。

早期菜谱场景中的类别、难度、菜名过滤不属于通用文档 RAG 默认逻辑，后续如需领域过滤，应以独立插件或显式 metadata filter 的方式加入。

## 8. 存储与运行边界

当前系统依赖：

- MySQL：主业务数据、会话、消息、文档元数据、计费记录。
- 本地文件：源 PDF、压缩后的 Markdown/JSON、模型文件、配置文件。
- 进程内索引：FAISS 与 BM25 索引文件位于 `rag.storage_dir`。
- 本地/远程模型：优先本地 llama.cpp 推理，可配置远程模型 fallback。

不建议在当前阶段引入 Redis。只有当出现跨进程高并发限流、异步任务队列、热点缓存或多实例部署需求时，再考虑 Redis。

## 9. 后续演进方向

短期优先级：

1. 稳定 PDF 压缩卡片的生成质量，并用固定检索问题做轻量回归。
2. 明确文档 visibility 命名，代码中的 `group` 可逐步迁移为更直观的 `tenant`。
3. 完善用户上传文档的状态机和失败重试。
4. 扩展 RAG 评测集，覆盖更多论文和租户文档查询。
5. 为 C++ 后端补充关键 service/repository 单元测试。

中长期可以考虑：

- 异步文档处理队列。
- 更强的论文结构化解析。
- 图 RAG 或实体关系索引。
- 多实例部署下的共享索引和分布式限流。

## 10. 原生 MCP 工具入口

C++ HTTP listener 同时提供 `/mcp/knowledge`、`/mcp/web` 和统一 `/mcp`，不需要独立
TypeScript 转发进程。McpService 处理 Streamable HTTP/JSON-RPC，ToolService 统一工具
定义、参数校验和执行；知识库调用 KnowledgeSearchService，网页搜索调用独立
WebSearchService（DeepSeek Search API）。REST 检索与 MCP 共用证据序列化。

认证复用后端 JWT。MCP 请求由有界工作线程池执行，响应回到连接所属的 muduo
EventLoop，避免远程搜索占用网络事件线程；出站 HTTP 使用 cpp-httplib，和入站
muduo I/O 分开。普通用户网页搜索权限、工具限流、上游并发和 Origin 白名单均可配置。

进程内 Agent Runtime 复用工具边界。远程模型聊天默认运行“模型 → 工具 → 模型”循环，
本地模型保留原有生成流程。旧网关保留兼容。详见 [原生 MCP](docs/native-mcp.md) 和
[Agent Runtime](docs/agent-runtime.md)。
