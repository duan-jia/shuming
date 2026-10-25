# 近期论文 Agent / RAG 测评数据 v0.1

**状态：已由 Ncodex 二次核对的 silver 标注，不是独立人工审定的 gold，也不是“模型从未见过”的证明。**

仅增加数据和离线辅助脚本，不改后端、前端、CI、数据库或部署。论文全文、提取文本、图像、账号和密钥不入 Git。

## 内容

| 内容 | 数量 / 范围 |
|---|---|
| 锁定版本的论文 | 30 篇：RDMA、分布式训练、网络/服务负载均衡各 10 篇 |
| 研究家族 | 28 组；Celeris/OptiNIC、Ofan/Load Balancing for AI Training 不作为独立家族 |
| RAG 问题 | 100 道：90 单篇、10 跨篇；70 中文、30 英文 |
| 单篇题证据深度 | 70 道摘要/首页基础题，20 道正文细节题；不能称为全文深度题集 |
| 正相关标注 | 110 条 query–paper 对；未标注文档 **不是负样本** |
| 证据定位 | 90 处，记录 PDF SHA-256、1-based PDF 页码、提取页哈希、文本范围哈希 |
| Agent 场景 | 30 个，含工具选择/参数/依赖、跨文档、分页、图像、网页、权限、超时、注入、隐私 |
| 二次核对记录 | 130 条；同一助手复核，**没有独立人工复核** |

结构：

```text
corpus/manifest.jsonl       仅此清单对应的论文可加入测评知识库
rag/queries.jsonl          问题、参考答案、证据 ID
rag/qrels.jsonl            已核对的正相关文档
rag/evidence.jsonl         可复现证据坐标；不附全文段落
agent/tasks.jsonl          用户任务、工具预算和验收规则（评审侧）
agent/fixtures.jsonl       自撰合成工具响应；不是论文原文或真实后端响应
agent/tool-contracts.json  对照 shuming 指定提交核对的参数约束
review/records.jsonl       每题/任务核对记录
review/decisions.md        修订、限制、后续验收
scripts/ tests/            下载、提取、校验、离线评分和单元测试
```

## 本地准备与校验

Node.js >=22.13、npm、curl；有原生 canvas 二进制支持的平台（本次验证 Linux x64、Node 22.23.3）。

```bash
cd datasets/recent-papers-v1
npm ci
npm test
npm run validate
npm run download       # 只下载锁定版本；SHA 不一致即停止，不自动替换
npm run extract        # PDF.js 6.4.299，逐页规范化提取
npm run render         # 在本地渲染 ByteScale PDF 第4页，供 agent-012 图像夹具使用
npm run validate:cache # 校验全部PDF、逐页文本和90处证据范围
```

默认缓存为本目录 `.cache/`，已忽略。可用 `EVAL_CACHE_DIR=/绝对路径` 指定单独缓存目录，各命令使用同一变量。只下载/提取论文，不连接数据库、不上传知识库、不删除已有数据。首次下载需要联网；已存在且哈希正确的文件跳过。损坏缓存保留给操作者检查，不自动覆盖。`extract` 对已有文本只检查绑定 PDF 哈希；要验证文本内容请运行 `validate:cache`。更换 PDF.js/字体/平台后文本可能改变，必须复核，不得悄悄换证据坐标。

证据偏移是规范化文本的 **JavaScript UTF-16 下标**，不是 PDF 字节、视觉字符或数据库 chunk ID。提取规则：PDF.js text items 用空格拼接，NFKC 归一化，再折叠空白。

## RAG 使用

只导入 `corpus/manifest.jsonl` 对应 PDF，**不要把题目、参考答案、qrels、证据文件或 Agent 合成响应索引进知识库**。导入时另存 `paper_id -> 当前数据库 doc_id` 映射；不能把本数据的逻辑 ID 或 mock 的 1001 当成真实数据库主键。检索结果先通过该映射转为逻辑 ID：

```json
{"query_id":"rag-001","doc_ids":["uccl","pcm"]}
```

一行一个问题，保存为 rankings.jsonl：

```bash
npm run score:rag -- /path/to/rankings.jsonl
```

输出 @1/3/5/10 的**已标注正例召回、命中、首个已知相关结果倒数排名**。同篇多次出现消耗检索位置，但不重复得分；缺失问题计零，拒绝未知 ID 和重复提交问题。分母是每题已标注正例数，**不是已穷尽的所有相关论文数**，不可直接当完整 gold Recall/nDCG，更不能把 hits/k 冒充 Recall。此版不评估生成答案，也没有经过穷尽排查的 RAG 无答案负例集。

请同时记录检索模式、chunk/parent 粒度、候选深度、切块策略、嵌入模型、索引版本及语料哈希。若要比较 chunk 级召回，需把 chunk 映射到这里的证据范围；本版没有声称实现该适配器。

## Agent 使用

1. 先用固定 mock 检查工具决策与恢复行为，再单独接真实模型/后端做 E2E。
2. 模型侧只提供任务的 `prompt`、身份/能力、可用工具的真实 schema，以及必要的 `fixture.setup`。**不要提供 required_tools、checks、参考答案、隐藏响应、qrels 或复核记录**。
3. `mock.mjs` 按参数匹配合成响应；默认补齐读取参数，优先匹配更具体规则，找不到/歧义时报错。工具错误通过 `error` 返回，不伪装成功。
4. 29 个夹具可使用自撰文本响应；`agent-012` 还需本地下载并渲染真实 PDF 页，图像输入模型由 harness 把本地 asset 编成图像消息。不能只把文件名给模型冒充视觉输入。
5. 网页 mock 是固定快照，不随互联网变化；``.invalid`` 下载/攻击地址均为合成不可用地址，不是真实服务或密钥。

归一化轨迹示例：

```json
{"task_id":"agent-001","calls":[{"name":"search_knowledge","arguments":{"query":"UCCL"},"result":{"hits":[{"paper_id":"uccl","doc_id":1001}]}},{"name":"fetch_knowledge","arguments":{"doc_id":1001,"representation":"text"},"result":{"doc_id":1001,"content":"..."}}],"answer":"..."}
```

```bash
npm run score:agent -- /path/to/normalized-traces.jsonl
```

只做机械检查：参数、权限工具集、调用预算、顺序、文档来源、部分分页/指定参数等。输出 `mechanical_pass` 后仍须按任务 `checks` 评审答案事实、引用、拒绝、隐私与注入防护；**不会输出“整体 Agent 已通过”**。无工具场景即使机械通过，也可能因没有正确拒绝而语义失败。

`fixtures` 使用固定 synthetic doc_id；真实后端必须生成本次导入映射，重绑定夹具并归一化响应后再评分。原生搜索响应形状不同于这里的 hits/results；本版**不包含真实模型调用器、后端轨迹适配器、数据库同步器或已完成的在线测评**。它们不是“提交数据”的隐含部署操作。不要直接把 mocks 接到生产授权层。

## 时间隔离、版权和实验边界

- 选择窗口为 2024-10-25 至 2026-10-25；清单记录的是 **arXiv 首次提交日期**，并非已证明的全网最早公开日期。
- 每篇 `training_membership=unknown`；目标模型/快照尚未指定，不宣称未进入训练。Ofan 的关联公开研究已在 2025-07-28 出现。
- `draft_unsplit` 是有意保留：不能把已用于调试的题直接标为正式隐藏测试。建立正式 train/dev/test 时按 `family_id` 分组，并把跨篇题涉及的家族一起分组；不能随机拆题制造泄漏。
- DLB 是服务请求级而非逐包均衡；Scaling Pretraining 是 BERT 式二进制代码预训练扩展，属于邻近系统研究。请分别报告子域，不夸称全是前沿自回归大模型训练或逐包路由论文。
- arXiv 预印本的事实/收益是**作者在指定实验中的报告**，不是本项目复现或同行评审认证；新近并不自动代表质量高。
- 清单保留每篇许可链接，**本仓库不重新分发任何 PDF、全文提取或页面图像**。公开可读不等于可任意转授权；下载者仍需遵守各论文许可。本项目仓库当前仍是私有，不会因添加这些文件而自动公开。
- 完成独立复核、候选负例池补判、无答案题核验、家族隔离、模型冻结及真实运行后，才能升级为正式 benchmark。单独公开数据并不能证明方法效果。
