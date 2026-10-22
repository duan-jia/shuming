# Agent Runtime

远程模型聊天默认使用进程内 Agent Runtime：模型选择工具，Runtime 校验权限并执行，
将结构化调用和结果追加到当前问题的上下文，再次调用模型。可以不使用工具直接回答，
也可以多次检索。MCP 和 Runtime 使用相同的 ToolService，内部不经过 MCP/HTTP 网关。

```text
POST /api/chat/stream
  → 身份、session 所有权、限流、额度检查
  → 有界推理队列
  → Agent Runtime
      → RemoteEngine（流式模型调用）
      → search_knowledge / search_web（需要时）
      → 结果回传模型 → 继续或完成
  → 汇总用量、结算、保存回答
```

## 模型与工具

当前工具调用适配器支持 OpenAI Chat Completions 格式的远程 SSE：累积按 index
分片的 tool_calls、保留 tool_call_id、解析 finish_reason 和 usage，并在后续模型
请求中保留当前工具轮次的 reasoning_content。遵循
[DeepSeek 工具调用](https://api-docs.deepseek.com/guides/tool_calls/) 与
[思考模式](https://api-docs.deepseek.com/guides/thinking_mode/) 的上下文要求。

可用工具由后端决定：

- `search_knowledge`：按认证用户的 tenant 检索。请求显式传 `enable_rag=false`
  时不提供此工具；Agent 默认提供知识库工具。
- `search_web`：需要开启 web_search，并通过已有 MCP 网页搜索权限策略。
  管理员可用；普通用户需 `mcp.allow_user_web_search=true`。

模型返回未知工具、越权工具、无效参数或执行失败时，Runtime 将错误作为 tool 结果
回传模型，让模型调整查询或说明限制。工具输出限制体积并作为不可信证据处理；提示词
要求引用文件名或 URL，禁止把私有文档正文和秘密发送给网页搜索。尚未实现跨工具的
数据流隔离；如需严格隔离，应禁用网页搜索，或另行增加租户级工具策略。

本地 llama.cpp 适配器尚不支持结构化工具调用，继续单次生成/RAG 流程。
`agent.enabled=false` 可让远程聊天也回到原有单次生成路径。

## 配置

```json
{
  "agent": {
    "enabled": true,
    "max_turns": 6,
    "max_output_tokens": 0,
    "max_total_tokens": 24000,
    "timeout_seconds": 90,
    "max_tool_calls_per_turn": 4,
    "max_tool_result_bytes": 24000
  }
}
```

- Agent 默认不发送 max_tokens，由模型服务端决定单轮输出上限。max_output_tokens
  为 0 表示不设置；正值或请求中的 max_tokens 可以显式指定（1～8192）。
  本地模型仍使用原有的默认生成上限。
- 普通文本因长度截断时，保留回答并返回 done/truncated=true，前端提示继续追问；
  不执行被截断的工具调用。只有 reasoning、没有可见回答的截断返回 output_limit。
- 轮数包括最终回答的模型调用。到最后一轮仍要求工具时，停止并返回 turn_limit，
  不再执行无机会读取结果的工具调用。
- 请求 token 预算累计每一轮的 prompt 和 completion；每轮前检查剩余用户额度，
  显式指定输出上限时根据输入估算缩小它；不指定时不注入上限，单轮消耗可能超过
  剩余预算，结束后停止后续调用。上下文裁剪只删除旧的完整问答对，保留本次工具链。
- 不为并发的不同 session 预留额度；逐轮额度检查不能消除不同请求之间的额度竞争。
  数据库仍通过既有事务和 request_id 执行最终结算。
- 推理队列最多等待 64 个请求，满时返回错误。同一 session 只允许一个活动请求，
  避免并发修改该对话的上下文。时间预算包含排队等待。
- 工具按序执行，单轮最多 4 次；知识库与网页工具复用 MCP 的用户限流。
- 模型和搜索使用带 watchdog 的 HTTP 请求，可中断阻塞读；本地 RAG/嵌入为同步计算，
  取消在计算边界生效。

## 流式事件与停止

仍使用 SSE `event: message`，JSON 的 type 可以是：

| type | 内容 |
|---|---|
| start | request_id |
| agent_turn | 当前模型轮次 |
| tool_start | name、tool_call_id |
| tool_end | name、tool_call_id、is_error |
| reasoning | 模型 reasoning 内容 |
| answer | 文本片段 |
| done | 累计 usage、计费/额度和持久化状态 |

错误使用 `event: error`，包含 error、stop_reason、已产生的 usage 和持久化状态。
stop_reason 包括 cancelled、timeout、token_limit、turn_limit、tool_limit、output_limit、model_error。
连接异常结束不会作为成功回答；模型流格式错误或缺少结束信号会返回错误。

前端显示工具状态和“停止生成”。停止按钮中止前端读取，并显式发送：

```http
POST /api/chat/cancel
Authorization: Bearer <用户 JWT>
Content-Type: application/json

{"session_id":"...", "request_id":"..."}
```

只有对应用户、租户、session 和 request_id 均匹配时才会取消。SSE 断线也会触发取消。
活动请求状态在结算完成后释放，不需要持久化取消状态。

## 用量与历史

成功、失败、超时和取消都会结算已消耗的模型轮次；有可见文本时保存一对
user/assistant 消息，避免已显示的部分回答完全丢失。工具中间结果只保留在本次运行，
不写入历史 messages；模型 reasoning 汇总写入已有 reasoning 列，并在下次问题的
assistant 历史中回传。旧消息没有 reasoning 时使用空字段，不需要数据库迁移。DeepSeek Search 的请求费用不计入模型 token 账单，使用工具
请求限流控制。

优先使用供应商 usage。流被取消或供应商未返回 usage 时，采用输入与已收到输出的
估算值，SSE 的 `usage.estimated=true` 会标明；该值不是供应商最终账单。未发出模型
请求、也未收到模型输出时不产生估算用量。

## 验证

```bash
cmake --build build --target fast-llm-reasoner fast-llm-cli native-mcp-test-server -j2
python3 tests/agent_runtime_e2e.py --server-command build/bin/native-mcp-test-server -v
```

使用真实 muduo、RemoteEngine、SSE 和 Runtime，加本地模型/搜索 fixture，覆盖多轮
工具链、tool_calls 分片、reasoning 回传、工具错误与权限、用量累计、预算与轮数、超时、
断线后释放工作线程，以及既有 MCP 回归。不依赖真实模型密钥或付费搜索权限。
