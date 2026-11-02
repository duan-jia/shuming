# Agent 会话历史与执行记录

Agent 仍使用 Chat Completions。每次用户请求的消息序列独立保存，后续请求从记录中恢复：

```text
user → assistant(tool_calls) → tool → assistant(tool_calls) → tool → assistant
```

## 存储与恢复

- `agent_requests`：请求 ID、会话 ID、配置模型名、状态、起止更新时间。
- `agent_events`：按自增 ID 排序的消息、模型请求/响应、工具开始/结束和请求结束记录。
- `messages`：继续保存供聊天页面展示的问答、思考和文件元数据；新增 `request_id` 关联执行记录，避免恢复时重复拼接展示问答。
- 工具调用意图先落库，再进入工具服务。模型请求也在发送前记录。持久化失败时停止后续模型或工具操作。
- 历史读取最近 100 个请求的完整消息，以及最多 200 条旧格式消息；上下文预算按完整请求裁剪，不拆开工具调用和结果。
- 旧格式消息仍可读取；助手上下文带有“未保留工具执行记录”的说明。旧会话的工具参数、返回和次数无法补造。
- 请求开始时保存附件身份和哈希；后续读取仍检查当前文档访问权、哈希和提取版本。工具返回的 PDF 页图不写入历史，保留文字说明，需要视觉证据时重新读取页图。

正常重启会恢复已保存的调用和返回。若进程在调用期间退出，未配对的调用在构造模型上下文时补充“无持久结果，执行状态未知”的结果占位。这不是实际执行结果；原始事件不会被改写，工具也不会自动重跑。

进程强制退出可能使请求状态停在 `running`；这表示请求没有完成落库，不能据此判断它仍在运行。当前实现没有跨实例租约或自动重试执行。

## 查询接口

先从会话的 SSE `start.request_id` 获取请求 ID，也可以列出最近 100 个请求：

```http
GET /api/sessions/{session_id}/requests
Authorization: Bearer <access_token>
```

读取指定请求的事件：

```http
GET /api/sessions/{session_id}/requests/{request_id}
Authorization: Bearer <access_token>
```

接口同时检查认证、路由权限和会话所有者。其他用户即使知道两个 ID，也不能读取记录；响应设置 `Cache-Control: private, no-store`。前端现有 API 转发可继续使用这些路径；本次没有新增页面面板。

| 事件 | 含义 |
|---|---|
| `message` | 用于恢复历史的结构化消息；`tool_calls` 与 `tool_call_id` 关联 |
| `model_request` | 实际组装的模型 HTTP JSON 正文的脱敏副本，含 `messages`、`tools`、模型与参数；不记录 Authorization |
| `model_response` | 解析后的模型文本、思考、工具调用、停止原因、错误与 token 用量；不是原始 SSE 字节流 |
| `tool_start` | 工具名、原始参数字符串、调用 ID，以及是否在执行前取消 |
| `tool_end` | 原始工具数据 `raw_result`、裁剪后模型可见文本 `model_content`、错误、阶段和耗时 |
| `request_end` | 来源文件、停止原因、错误和总用量 |

`tool_end.execution_started=true` 仅表示已进入工具服务，不保证完成；结合 `is_error` 和 `stage` 判断。阶段包括权限、限流、参数、执行和结果处理。

SSE 继续发送轻量 `tool_start/tool_end`；详细数据通过上述接口查询。结束事件新增 `trace.persisted`，不要把它与 `messages.persisted` 混淆。完整 trace 属于独立于聊天展示的数据。

## 数据边界

- 签名下载链接、明确的凭据字段和图片 Base64 在持久化前脱敏；模型当轮接收的原始请求不因脱敏而改变。因此 trace 是语义级脱敏副本，不是字节完全相同的网络抓包。
- 消息和工具内容可能含私有资料；按会话权限访问，数据库备份也需要相同保护。
- 单个事件最多 8 MiB，超过时显式失败，不静默保存截断 transcript；工具上下文仍受 `agent.max_tool_result_bytes` 限制。
- 本版本没有自动摘要或定时保留策略。旧请求按预算整体移出模型上下文，数据库记录保留到会话被删除；删除会话会级联删除其执行记录。
- `source_files` 仍是检索命中或附件列表，**不证明回答引用或受到这些文件支持**。`request_end.source_files_role` 明确记录这一边界；引用质量及页面标签调整属于独立改动。

## 升级

新数据库使用 `sql/init.sql`。已有数据库先备份，再执行：

```sh
mysql --database=llm_platform < sql/migrate_agent_history.sql
```

凭据通过现有安全方式提供，不写入命令或版本库。迁移增加 nullable 列和新表，可重复执行，不改变旧问答或文档。staging 的迁移检查也包含这些新增结构。缺少 schema 时新后端拒绝启动并提示迁移文件，不静默降级。

回退旧后端时可保留新增列和表；不要为回退删除记录。后端启动不会自动执行迁移，部署前需要显式升级数据库。

## 验证

```sh
cmake --build build --target fast-llm-reasoner native-mcp-test-server conversation-history-test agent-journal-test auth-security-test -j4
./build/bin/conversation-history-test
./build/bin/auth-security-test
python3 tests/agent_runtime_e2e.py --server-command build/bin/native-mcp-test-server
python3 -m unittest discover -s tests -p 'test_staging_migrations.py'
```

完整 HTTP/MySQL 测试使用专用临时数据库，以 `sql/init.sql` 初始化，然后运行：

```sh
MYSQL_TEST_HOST=<isolated_mysql_host> MYSQL_TEST_PASSWORD=<test_only_password> \
  python3 tests/agent_history_e2e.py
MYSQL_TEST_HOST=<isolated_mysql_host> MYSQL_TEST_PASSWORD=<test_only_password> \
  ./build/bin/agent-journal-test
```

该测试启动真实 C++ 后端和本地 mock 模型/网页搜索，验证实际请求 JSON、工具返回、后端重启、取消、强制崩溃与跨用户拒绝。它会写入测试账号和会话，必须使用隔离数据库，不能连接现有业务数据库。PDF 回归还需要 Poppler。

## 导出和固定证据重放

`scripts/replay_agent_trace.py` 只依赖 Python 标准库。导出需要会话所有者的访问 token，通过环境变量传入；也可显式使用 `--login-config <本地配置>` 登录管理员自己的会话。输出目录必须是新目录或空目录，文件权限为 0600，凭据、签名链接和图片数据会脱敏。

```sh
python3 scripts/replay_agent_trace.py export \
  --base-url http://127.0.0.1:3002 \
  --session-id <session_id> --request-id <request_id> \
  --output .tmp/trace-export
```

token 默认从 `EMOS_TRACE_TOKEN` 读取。省略 `--request-id` 时读取该会话最近一次请求。输出包括 `trace.json`、每轮实际模型输入 `model-request-NN.json` 和可阅读的 `report.md`。旧会话没有工具 trace，不能用问答或来源文件补造。

已有 trace 文件可离线查看，不访问网络：

```sh
python3 scripts/replay_agent_trace.py inspect .tmp/trace-export/trace.json \
  --output .tmp/trace-inspect
python3 scripts/replay_agent_trace.py replay .tmp/trace-export/trace.json \
  --compare-without-tool-history --output .tmp/trace-replay-dry
```

真正重新生成回答必须显式添加 `--live` 和匹配模型的配置：

```sh
python3 scripts/replay_agent_trace.py replay .tmp/trace-export/trace.json \
  --model-config .tmp/dev-eval-local/config.json --live \
  --compare-without-tool-history --max-tokens 2048 \
  --output .tmp/trace-replay-live
```

默认选择最后一个模型请求，也可用 `--turn 1` 指定轮次。脚本固定已记录的工具结果，设置 `tool_choice=none`，只调用配置中的模型，不重新执行工具。输出 `*.request.json`、`*.response.json` 和 `replay.md`；该生成不写回业务会话，不经过应用账单结算，但上游模型正常计费。仍可能生成错误描述，需要对照原 trace 判断。

API 密钥若仅由 Docker 环境变量注入，本地配置文件不包含密钥，需要在持有该环境变量的运行环境中执行，不要把密钥复制到命令行或导出文件。精简后端镜像中 Python 的默认 CA 路径可能不存在；可设置 `SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt`，使用与 C++ 后端相同的信任文件，保持证书校验开启。

对照模式移除历史工具调用及返回，保留已有普通问答；这是消融试验，不是对旧故障的精确复现。重放还会改成非流式请求并设置输出上限，不能声称与原始请求完全相同。包含已省略页图的请求会拒绝在线重放。私有资料只有在明确授权后才应使用 `--live` 发送到所选模型。
