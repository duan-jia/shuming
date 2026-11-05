# 文档治理与分类浏览

文档管理负责查看、分类、审核和元数据校正。支持按论文名字匹配，以及发表年份、归一后的会议/期刊类型组合筛选；不做内容语义相关性排序。查找相关论文、理解研究问题及基于论文回答均由主对话中的 RAG 完成。

分类属于已有论文卡片元数据。上传后的文档治理流程为 PDF 文本提取 → 论文卡片生成与发表信息提取 → 字段校验、待审核判定 → 保存 JSON/Markdown → 入库。`scripts/paper_card.py` 在生成卡片时一并提取并校验 `publication_year`、`publication_type`（`conference` / `journal` / `preprint` / `unknown`）和 `publication_venue`，不另外发起模型调用。文档管理只读取已保存元数据，不在打开页面或切换分类时重新提取。

旧卡片的 `venue_and_year` 保留兼容读取；只接受无歧义的单个年份，类型仅根据明确的会议/期刊文字识别，无法确认时显示未分类。上传时间不用于推断发表年份。缺失信息可在治理环节补充或人工校正，不为补齐分类自动向外部模型重发历史论文。

所有登录用户（含游客）可编辑自己可见文档的元数据 JSON。文档可见性仍由后端判断，不能访问其他组的私有文档。`GET /api/library/documents` 首先通过后端文档可见性检查，再读取这些文档的本地论文卡片。`GET/PUT /api/library/documents/:id/metadata` 读取和保存完整书目 JSON，保留自定义字段；源文件身份、处理状态和生成溯源由系统保留。会议信息去除届次、年份、地点，例如 SIGCOMM 的不同写法统一成 `ACM SIGCOMM`。

人工校正覆盖保存在 `PDF_PROCESSED_DIR/library/<document_id>.json`，读取时优先于自动提取值，通过临时文件加原子重命名写入，不覆盖原始论文卡片或重建 RAG 索引。不需要 SQL 迁移；备份和恢复必须包含完整 `PDF_PROCESSED_DIR`，并与对应数据库一起恢复。后端与前端需同时更新，以便列表返回卡片存储名。历史文档缺少卡片时仍可显示、按已有分类浏览及人工校正。

验证：

```bash
cd web
node --experimental-transform-types --test tests/paper-library.spec.ts tests/paper-upload-naming.spec.ts tests/library-access.spec.ts
npx tsc --noEmit
```

文档库默认进入“总库”，汇总当前用户有权查看的所有文档；可切换 EMOS group（公共库）及 RDMA group（当前研究组库）。游客仍使用原测试组范围。总库支持名字和分类筛选，所属分库在每条记录上显示；上传须先选择对应分库。

上传交互：汇总标签为 All。普通用户上传目标固定为自己所在研究组。管理员在上传按钮旁选择 EMOS group 或 RDMA group，默认 RDMA group；切换分库同步目标，回到 All 保留已选目标。实际入库权限仍由后端校验。

显示名称与文件名分离：文档列表、名字筛选及审核/删除提示优先使用论文卡片的 `title`，保留冒号等原标题标点，仅合并换行及多余空白。人工编辑元数据 `title` 后列表同步更新。旧文档没有有效标题时，回退到去扩展名的 PDF/Markdown 文件名；不从下划线猜测原标点。PDF、Markdown 和 JSON 继续使用现有安全文件名与存储关联，不因标题校正重命名或重建索引。

统一文件命名规则（`web/src/lib/paper-filenames.ts`）：

- 下载名：`<安全标题>.pdf`、`<安全标题>.md`；二者取同一个主干，优先使用入库时的源文件名。
- 磁盘名：`<UUID>--<安全标题>.pdf/.md/.json`；三个文件共享 UUID 和主干。UUID 区分同名论文，SHA-256 仍用于去重。
- 安全标题使用 NFKC 规范化、合并空白，将 Windows 非法字符及控制字符替换为 `_`，去除尾部点和空格；Windows 设备保留名加 `_` 前缀，空名称回退到 `document`。
- 主干最多 160 个 UTF-8 字节，按完整 Unicode 字符截断，并再次去除尾部点和空格。页面标题不受这个长度限制。
- 上传、下载名生成共用清理函数；下载、删除和元数据读取共用存储名校验。兼容历史纯 UUID 及后端允许的最多 180 字节主干，不重命名已有文件。
