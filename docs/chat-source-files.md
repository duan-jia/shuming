# EMOS 回答末尾的来源文件

知识检索后，在回答末尾显示紧凑的 PDF 下载控件。默认显示两个文件，其他折叠；长文件名省略，可悬停查看完整名称。没有可下载文件时不占位。沿用现有暗色对话界面，不增加卡片、头像或标题栏。

## 数据链路

- Agent 收集成功的 `search_knowledge` / `fetch_knowledge` 响应中实际进入上下文的文档 ID。超出工具上下文预算而被删除的结果不收集；网页不作为本地 PDF。
- 非 Agent 的普通 RAG / 游客公开知识检索同样收集来源。
- 服务端重新检查文档可见性和源 PDF 是否可打开，再输出 `{doc_id, filename}`。按 ID 去重，Agent 每轮回答最多保留 40 个文件。
- `done` / 推理错误 SSE 增加 `source_files` 数组；历史消息接口也返回该字段。利用既有 `messages.rag_references` JSON 列保存引用，无新增表或迁移。旧记录为 NULL 时返回空数组，不追溯生成历史来源。
- 文本答案与来源引用分离；来源列表表示检索来源，不表示每份论文均直接支持每项结论。模型仍应在正文中准确引用证据。
- 只存稳定 ID/文件名，不存 JWT、存储路径或短时签名 URL。MCP 的原有 source 临时下载接口保留给外部客户端。

## 下载及权限

EMOS 点击控件时，通过已有 `/api/documents/:id/download/pdf` 接口发送当前 Bearer 登录凭证，重新检查权限；凭证不放进 URL。文档管理和回答来源复用同一个下载函数。权限失效、源 PDF 缺失或文档删除会在来源行下显示简短提示。

此接口沿用现有部署前提：Next 服务的 `PDF_SOURCE_DIR` 必须能够访问文档管理中的源 PDF（与后端源文件目录对应）。本改动不部署服务、不创建文件同步机制，也不改动数据库凭证或 CI workflow。

## 验证

```sh
cd web && npm ci && npm run build && npm run lint
cd .. && node --test tests/source_files.test.cjs
# 使用项目依赖的 nlohmann/json、spdlog 头文件路径：
c++ -std=c++17 -Iengine/include -I<json-include> -I<spdlog-include> \
  tests/source_files_test.cpp engine/src/services/knowledge_fetch_service.cpp \
  engine/src/server/http_protocol.cpp -lcrypto -pthread -o /tmp/source-files-test
/tmp/source-files-test
```

C++ 聚焦测试涵盖去重、40 文件上限、无效 ID、工具错误、外部 URL 不入来源、租户可见性、公开范围、源文件缺失和 PDF 校验。Node 测试涵盖输入校验、去重、下载鉴权与失败处理。

本次另用真实 Chromium 对构建后的 EMOS 做了桌面 1280、平板 768、手机 390 像素检查：折叠、长文件名、无水平溢出、历史切换、SSE 完成后显示、下载及拒绝提示。该浏览器验证使用模拟后端和测试 PDF，不等同于真实模型/MySQL 的端到端验收；完整后端链接构建、数据库实际写入/重启恢复和真实推理仍需集成环境验证。
