# 评测与回归测试

当前只使用 [recent-papers-v1](datasets/recent-papers-v1/README.md) v0.1.4：30 篇冻结论文、100 条 RAG 查询、30 个 Agent 场景。旧菜谱和历史 RDMA 小集合已移除。

| 维度 | 输入 | 评分 |
| --- | --- | --- |
| Agent 工具调用 | `datasets/recent-papers-v1/agent/tasks.jsonl` | 参数、工具权限、预算、依赖顺序、成功读取、逐次分页、图像页范围等机械检查；答案语义另审 |
| RAG 文档检索 | `rag/queries.jsonl`、`rag/qrels.jsonl` | 已知正例召回；精确率上下界；满足标注完整条件时才给出精确 P/R |

数据源已正式纳入当前分支，PDF/全文/图像、服务凭据和模型输出仍不入 Git。源清单、证据哈希、版本和复核历史均随数据保存。

## 本次修复

- 失败搜索中的文档 ID、URL 不得给后续读取提供成功证据，统一处理 error/is_error/isError；有效重试仍可继续。
- 第 4 页图像必须来自同一次成功的目标文档 pages 读取及返回图像，不能把第 1 页图像和第 4 页文本拼接通过。
- 合入 `rag-084 → ofan` 的漏标与第 2 页证据，此前为111条正相关；本轮进一步补判到3000个组合：122相关、2878不相关、100处证据。
- 本轮完成固定30篇语料的全组合助手复核，详见[闭集复核记录](datasets/recent-papers-v1/review/closed-corpus-v0.1.4.md)。评分器仍支持未来版本保留未判定状态。正常实时任务与受控失败场景分别标记。

## 文件组织

| 位置 | 职责 |
| --- | --- |
| `rag_eval.py` | 实时采集或离线评分RAG排名 |
| `agent_model_eval.py`、`agent_fixture_bridge.mjs` | 真实模型调用循环及受控工具执行 |
| `agent_eval.py` | 对完整Agent轨迹进行机械评分 |
| `eval_common.py` | HTTP认证与报告输出等公共功能 |
| `datasets/recent-papers-v1/` | 唯一正式数据源、证据、评分器及其Node回归 |
| `reviews/` | 历史审查记录及错误轨迹复现 |
| `reports/` | 可提交的测评结论与汇总，不包含原始模型输出 |
| `test_evaluation.py`、`test_agent_model_eval.py` | 入口、协议与模型执行器的本地回归 |
| `native_mcp_e2e.py`、`agent_runtime_e2e.py` | 真实C++服务配合受控provider的功能回归 |
| 其他 `test_*.py` | 部署、隔离、发布及论文卡片回归 |

原始响应、完整运行轨迹和中间产物统一放在被Git忽略的 `.tmp/evaluation/`。论文PDF及提取文本使用独立缓存，不与题目/标签一起索引进知识库。

## 已运行结果

本次实际结果见 [2026-10-25测评报告](reports/2026-10-25-model-evaluation.md)：100题真实本地RAG与真实DeepSeek模型/受控工具场景分开报告。

## Agent 评分

先由执行器运行任务，保存包含参数与结果的归一化 JSONL 轨迹，再评分：

```bash
python3 tests/agent_eval.py --predictions /path/to/normalized-traces.jsonl
```

真实模型＋受控响应（使用DEEPSEEK_API_KEY，生成原始provider返回与完整轨迹）：

```bash
python3 tests/agent_model_eval.py --model deepseek-chat --output .tmp/evaluation/new-agent-run
python3 tests/agent_eval.py --predictions .tmp/evaluation/new-agent-run/normalized-traces.jsonl
```

端点默认 https://api.deepseek.com/v1；这会发送题目与合成工具响应至该模型服务。`--supports-images` 仅在目标模型确实支持图像时启用，页面资产由 EVAL_CACHE_DIR 提供。图像能力不足的必需场景明确跳过，仍计入全题分母。故障或超过6轮等未正常完成的任务保留执行状态，不作为完成轨迹提交，计零。

输入格式和 mock 用法见 [数据集说明](datasets/recent-papers-v1/README.md)。默认报告 `.tmp/evaluation/agent.json`。遗漏任务计零并列出；有遗漏或机械失败时包装命令返回 1。

**当前后端 SSE 的工具名称事件不足以建立完整轨迹**，不能把它直接拿来判参数、读取对象或证据正确性。`tests/agent_model_eval.py` 已支持真实模型配合受控工具，完整后端轨迹适配器仍未接入。超时、空结果、403、恶意正文、无工具等场景需要执行器落实受控响应及工具集合；不能把 prompt 发给普通线上账号就视为运行了对应场景。

`mechanical_pass` 仅表示轨迹合同通过，不是答案正确或总体 Agent 成功。模型只能看到 prompt、能力、真实工具 schema 及必要 setup，不能看到隐藏答案、required_tools、checks、qrels 或预设响应。

## RAG 评分

离线输入为一行一个查询的逻辑论文 ID 排名：

```json
{"query_id":"rag-001","doc_ids":["uccl","pcm"]}
```

```bash
python3 tests/rag_eval.py --predictions /path/to/rankings.jsonl --k 1 3 5 10
```

真实检索需要专用评测账号 JWT，以及语料导入时保存的映射。映射格式为 `{"papers":[{"paper_id":"uccl","doc_id":4,"pdf_sha256":"锁定PDF哈希"}, ...]}`，必须覆盖全部 30 篇且 ID 唯一，哈希与清单一致。

```bash
read -rs EMOS_EVAL_TOKEN
export EMOS_EVAL_TOKEN
python3 tests/rag_eval.py \
  --base-url http://127.0.0.1:9007 \
  --document-map /path/to/paper-doc-map.json \
  --mode parents --k 1 3 5 10
```

后端默认端口取决于实际部署，以上 9007 是当前独立开发环境示例。此命令只调用检索工具，不生成答案、不创建聊天会话；嵌入或查询改写仍可能调用已配置服务。PDF 哈希映射是导入记录，调用前仍应核实当前服务使用同一导入语料与索引。

默认报告 `.tmp/evaluation/rag.json`。请求失败按空排名计入分母，单独列出 error 并返回 1；缺失离线题目也返回 1。未知文档不得悄悄丢弃，未知逻辑 ID 被拒绝；可用 `--exclude-document-id` 明确列出语料外联调文档，记录原始排名与所有排除项后生成闭集排名。实时返回映射外文档时该题记错误，防止过滤后虚高得分。使用独立评测语料或明确处理外部干扰文档后再比较结果。

指标口径：

- 排名按接口顺序保留；重复文档消耗位置，但只获得一次相关命中。chunks 模式也按文档 ID 得分，重复 chunk 不被压缩以抬升后续排名。
- `known_positive_recall` 的分母是已标注正例数量，不是所有真实相关文档。
- `precision_lower_bound` 把未判定文档暂按不命中计算，`precision_upper_bound` 把它们暂按命中计算；不足 K 的空位置不计命中。
- `precision` 在前 K 个返回文档都已判定时才有值，否则为 null。
- `recall` 在该题全部 30 篇均有显式正/负判定且 qrels_complete=true 时才有值，否则为 null。只修改 complete 标记会被拒绝。
- 宏平均保留未知值，不剔除未判定题来制造完整精确指标。当前固定30篇闭集已全部判定，P/R可计算；数据仍是助手复核的silver，尚未独立人工复核或完成正式留出集划分。

## 验证

这些检查不需要 API 凭据，不调用付费模型：

```bash
node tests/datasets/recent-papers-v1/scripts/validate.mjs
node --test tests/datasets/recent-papers-v1/tests/dataset.test.mjs
node tests/reviews/probe_recent_papers_grader.mjs
python3 -m unittest discover -s tests -p 'test_evaluation.py'
python3 -m unittest discover -s tests -p 'test_agent_model_eval.py'
```

缓存可用时验证锁定 PDF、页文本及证据范围（默认 `.cache`，可指定共享缓存）：

```bash
EVAL_CACHE_DIR=/path/to/eval-cache \
  node tests/datasets/recent-papers-v1/scripts/validate.mjs --with-cache
```

原有 C++、原生 MCP、Agent runtime、部署隔离和论文卡片回归继续保留。受控回归通过不代表真实模型质量已经测出。原始复核记录见 [2026-10-25-review.md](reviews/2026-10-25-review.md)，其中 v0.1.2 的问题已在当前 v0.1.4 修复。

真实检索可用 `--interval 1.15 --checkpoint /path/to/checkpoint.jsonl --progress-every 10` 控制速率并逐题记录。限流重试只检查服务错误，论文正文中的 rate limit 不触发重试。


## 用户每日额度回归

`user-quota-test` 使用真实 MySQL 验证租户继承、用户覆盖、超过 INT 上限的额度、
跨日沿用、恢复继承、下调不清空用量、并发计费与修改额度。
仅对已经执行 `sql/init.sql`（或旧库完成相关迁移）的隔离测试数据库运行：

```bash
cmake --build build --target user-quota-test auth-security-test
MYSQL_TEST_HOST=127.0.0.1 MYSQL_TEST_PORT=3306 \
  MYSQL_TEST_USER=root MYSQL_TEST_PASSWORD=... MYSQL_TEST_DATABASE=llm_platform \
  ./build/bin/user-quota-test
./build/bin/auth-security-test
```

未设置 `MYSQL_TEST_HOST` 时测试以退出码 77 跳过。测试会创建并删除独立租户和用户；
管理路由的 admin/user/guest 权限矩阵由 `auth-security-test` 覆盖。

管理接口端到端回归（不调用模型，要求管理员 token；会创建并清理一个临时用户）：

```bash
EMOS_EVAL_TOKEN=... python3 tests/user_quota_api.py --base-url http://127.0.0.1:3002
```


## 对话 PDF 附件回归

```bash
python3 -m unittest discover -s tests -p 'test_pdf*.py'
EMOS_TEST_PASSWORD=... python3 tests/pdf_attachment_e2e.py \
  --base-url http://127.0.0.1:13022 --fixture-directory /path/to/isolated-fixtures
```

端到端测试只针对独立 MySQL、独立数据目录和本地模拟模型运行。
fixture-directory 包含模拟模型写入的 requests.jsonl 和共享 data 目录。
覆盖上传去重、逐页缓存、模型请求包含完整原文、追问和附件持久化、
数据库消息不保存正文、跨租户拒绝及无效 ID。不会调用外部模型。


## 用户管理与认证会话

```bash
node --experimental-strip-types web/tests/auth-client.test.mjs
cmake --build build --target auth-service-test auth-security-test
./build/bin/auth-service-test
./build/bin/auth-security-test
EMOS_TEST_PASSWORD=... python3 tests/auth_sessions_e2e.py \
  --base-url http://127.0.0.1:19028 --mysql-container isolated-mysql-container
```

HTTP 测试仅针对隔离数据库，配置 JWT 有效期为 4 秒；可选 mysql-container 用于强制会话绝对过期测试。
覆盖管理员创建/重置、自主改密、密码版本撤销、真实 access token 到期续期、刷新重放及并发、刷新/退出竞争、
删除用户、游客权限及 MCP 撤销；不会调用模型。前端单元回归验证并发只刷新一次、401 重试、断网保留身份和游客不续期。
