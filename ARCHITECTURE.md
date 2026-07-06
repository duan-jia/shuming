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
services    业务编排层，处理认证、对话、文档、管理等业务流程
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
- `users`：用户，属于某个 tenant，角色为 `admin` 或 `user`。
- `sessions`：一次对话会话。
- `messages`：会话消息，保存 user/assistant 内容、reasoning、RAG 引用和 token 统计。
- `documents`：导入后的文档元信息与内容，当前 visibility 使用 `public` / `group`，其中 `group` 等价于业务语义中的 tenant 可见。
- `document_chunks`：文档切块后的检索单元。
- `user_token_usage_daily`：用户每日 token 用量聚合。
- `token_usage_records`：每次请求的 token 使用明细，使用 `request_id` 保证幂等。
- `audit_logs`：关键操作审计。

文档权限模型应保持简单：

```text
public：全系统用户可见
tenant/group：仅同一 tenant 用户可见
```

当前不设计 private 个人知识库。

## 4. 对话请求流程

典型对话请求由 `/api/chat/stream` 进入：

```text
前端 SSE 请求
↓
chat_routes 鉴权并解析参数
↓
ChatService 加载 session、历史 messages、检查 token 限额
↓
如果启用 RAG，则调用 DocumentStore 混合检索
↓
构造 prompt
↓
ModelManager 选择本地 LLM 或远程 fallback
↓
SSE 流式返回 assistant token
↓
同步写入 messages、token_usage_records、user_token_usage_daily
```

当前系统规模约 500 人，优先使用 MySQL 同步落库即可。这样事务边界清晰，排障简单，也避免 Redis 扣费与异步入库带来的补偿复杂度。

## 5. Token 计费与幂等

token 限额以用户为主，租户表中的 `token_limit_daily` 表示该 tenant 下用户的默认每日额度。

计费相关责任划分：

- `messages`：记录对话内容及本次消息的 token 统计。
- `token_usage_records`：记录一次请求的计费明细和状态。
- `user_token_usage_daily`：记录用户当天累计使用量，用于快速判断是否超限。

幂等通过 `token_usage_records.request_id` 唯一约束保证。同一请求重复提交时，不应重复扣减。扣费、记录明细和消息落库应尽量放在同一事务或明确的顺序控制中，保证“有记录可追踪、失败可恢复”。

## 6. RAG 文档导入流程

当前 RAG v2 采用 document_card-only 策略：不把 PDF/PPT/DOCX 全文直接入库，而是先生成轻量 `document_card`，再把 card 导入 RAG。

`document_card` 的主要语义字段使用英文，包括 title、keywords、summary、main_contribution、useful_for 和 questions_answered；同时保留 aliases，用于中文姓名、中文标题、拼音、缩写和原始文件名。这样可以减少 BM25 对中文分词的依赖，同时保留实验室场景中常见的人名和中文题名入口。

后台预导入流程：

```text
原始文件：data/public 或 data/tenants/RDMA
↓
生成卡片：data/document_cards/public 或 data/document_cards/tenants/RDMA
↓
生成治理清单：data/governance/public 或 data/governance/tenants/RDMA
↓
批量导入：scripts/import_rag_manifest.py
↓
写入 documents/document_chunks，并更新 FAISS + BM25
```

`data/governance/...` 的作用是治理中间层：检查哪些源文件已经生成 card，输出导入清单和治理报告，避免导入脚本盲扫目录。`data/public` 导入为 `public`；`data/tenants/<tenant>` 导入为当前代码中的 `group`，业务语义是 tenant 内共享。

租户上传文档流程应与后台导入复用同一套后端文档模型，但导入时设置为 tenant/group 可见：

```text
用户上传文件
↓
保存 document 记录，status=processing
↓
生成 document_card
↓
切块、向量化、写入 document_chunks
↓
更新 FAISS/BM25
↓
status=ready
```

## 7. 检索架构

RAG 检索由 `engine/src/rag/` 负责，采用混合检索：

- QueryRewriter：将用户中文或混合语言 query 重写为英文检索 query，并保留 aliases 与原始 query；失败时回退原始 query。
- FAISS：对 card/chunk 内容做向量召回。
- BM25：对英文关键词、缩写、aliases 和必要中文姓名做词面召回。
- 权限过滤：检索时只返回 `public` 文档或当前用户 tenant 下的 `group` 文档。
- DocumentStore：统一管理文档、chunk、向量索引和 BM25 索引。

因为当前只导入 `document_card.md`，aliases、summary、keywords、main_contribution、questions_answered 等增强字段都会参与向量检索和 BM25 检索。该策略适合“找某个师兄做过什么”“哪篇文档提到某方向”的实验室检索场景。

早期菜谱场景中的类别、难度、菜名过滤不属于通用文档 RAG 默认逻辑，后续如需领域过滤，应以独立插件或显式 metadata filter 的方式加入。

## 8. 存储与运行边界

当前系统依赖：

- MySQL：主业务数据、会话、消息、文档元数据、计费记录。
- 本地文件：原始资料、document cards、模型文件、配置文件。
- 进程内索引：FAISS 与 BM25 索引文件位于 `rag.storage_dir`。
- 本地/远程模型：优先本地 llama.cpp 推理，可配置远程模型 fallback。

不建议在当前阶段引入 Redis。只有当出现跨进程高并发限流、异步任务队列、热点缓存或多实例部署需求时，再考虑 Redis。

## 9. 后续演进方向

短期优先级：

1. 将当前 English document_card + aliases RAG 流程作为稳定主线，持续用 `validate_rag_import.py` 回归。
2. 明确文档 visibility 命名，代码中的 `group` 可逐步迁移为更直观的 `tenant`。
3. 完善用户上传文档的状态机和失败重试。
4. 扩展 RAG 评测集，覆盖更多学生、论文和 public 文档查询。
5. 为 C++ 后端补充关键 service/repository 单元测试。

中长期可以考虑：

- 异步文档处理队列。
- 更强的论文结构化解析。
- 图 RAG 或实体关系索引。
- 多实例部署下的共享索引和分布式限流。
