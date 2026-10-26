# 论文 PDF 上传与命名

当前上传处理面向可提取文本的论文 PDF。处理器从论文正文提取原始标题，合并标题中的换行，并生成来源约束的论文卡片；缺失标题、非字符串标题或临时 UUID 标题会使处理失败，避免将临时文件名写入文档库。

## 文件与下载名称

- 文档显示名和 Markdown 下载名：`论文标题.md`。
- PDF 下载名：`论文标题.pdf`。
- 实体 PDF：`UUID--论文标题.pdf`；Markdown、JSON 处理产物使用同一存储前缀，避免同名论文覆盖。
- 文件名执行 NFKC 归一化，将路径分隔符、控制字符和跨平台不合法字符替换为下划线，标题限制为 160 个 UTF-8 字节，截断时保留完整字符。
- PDF 字节保持不变；查重继续使用原始 PDF 的 SHA-256。存储名称不作为文档 ID 或评测逻辑 ID。

论文卡片的模型请求限制 JSON 输出预算。请求官方 DeepSeek 接口时显式设置 `thinking.type=disabled`，避免输出预算耗尽后没有可用摘要和标题；其他提供方沿用通用请求结构。

## 失败后的文件处理

模型处理或提交前失败时清理临时文件；后端明确返回 4xx 拒绝时清理本次上传产物。提交开始后出现网络错误、超时或 5xx 时保留源文件及处理产物，因为数据库可能已经提交。用户重试时先按 PDF 哈希查重，避免重复建档。运维排查遗留文件时应先核对数据库引用，不应仅凭一次失败响应删除 PDF。

库范围与角色权限见 [实验室权限与文档库](permissions.md)。

## 回归验证

```bash
python3 -m unittest discover -s tests -p test_paper_card.py
python3 -m unittest discover -s tests -p test_pdf_model_request.py
cd web
node --experimental-transform-types --test tests/paper-upload-naming.spec.ts
```

测试覆盖标题缺失/UUID/换行、DeepSeek 与其他接口的请求差异、标题命名与元数据同步、提交响应丢失后保留文件，以及明确拒绝后的临时文件清理。CI 同步执行这些测试。
