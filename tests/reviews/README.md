# 复核记录

当前数据及全部相关性判定以 [`../datasets/recent-papers-v1/`](../datasets/recent-papers-v1/README.md) 为准。

此目录保留整理过程中已发现问题的历史证据，不能作为第二份当前标注：

| 文件 | 用途 |
| --- | --- |
| `2026-10-25-review.md` / `2026-10-25-audit.json` | v0.1.2及旧RDMA集合的审查发现；对应问题已处理 |
| `recent-papers-qrels-additions.jsonl` | 最早确认的Ofan漏标提案，已合入正式qrels |
| `probe_recent_papers_grader.mjs` | 独立复现两个历史误通过；当前评分器应拒绝这些轨迹，退出0 |

当前闭集复核、逐题证据与矩阵见数据集的 [`review/closed-corpus-v0.1.4.md`](../datasets/recent-papers-v1/review/closed-corpus-v0.1.4.md)。历史记录保持原意，不把修复前的发现改写成从未发生。
