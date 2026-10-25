# 本地 Docker 测试

> 新安装默认生成服务改为Qwen3.8-27B，见[模型指南](../models/README.md)。本文件的已有Compose网络不自动迁移：容器里的127.0.0.1不是模型宿主机。下文DeepSeek说明仅为显式云服务替代方案，不能视为当前默认配置。

从仓库根目录执行。需要 Docker Compose、Python 3；首次构建需要访问 Ubuntu、npm、GitHub 和 Hugging Face。

```bash
python3 deploy/local/prepare.py
docker compose -f deploy/local/compose.yml up -d --build
```

`prepare.py` 在 CPU 构建容器中编译当前源码的后端和 CLI，把运行所需的二进制、动态库和 CA 证书放在 `.tmp/local-runtime/`。首次构建器镜像为 `shuming-local-builder`；已有兼容 Ubuntu 24.04 构建环境可用 `--builder-image <image>` 复用。嵌入模型使用与 CI 相同的固定版本和 SHA-256 校验。脚本重复运行保留原有凭据和数据。

- 前端：http://localhost:3001
- 后端健康检查：http://localhost:9006/api/health
- 登录用户：`admin`。随机密码读取 `.tmp/local-deploy/config.json` 中的 `admin.default_password`。
- 端口可在 `deploy/local/.env` 修改。npm 连接受限时，可在该文件设置 `NPM_REGISTRY=https://registry.npmmirror.com`；Debian 下载较慢时可设置 `DEBIAN_MIRROR=mirrors.ustc.edu.cn`。MySQL 仅在 Compose 网络中开放。
- 前端使用 Next.js standalone 生产构建，API 代理在构建时指向 `http://backend:9006`；修改该地址需要重新构建。

本地配置和文件数据在 `.tmp/local-deploy/`，MySQL 在 `shuming-local_mysql-data` 卷中，均独立于现有部署。配置与 `.env` 权限为 `0600`，不提交 Git。前端包含 Python 与 pypdf，可执行论文处理脚本。

默认可测试登录、会话、Markdown 文档入库和真实 BGE 检索。DeepSeek 问答、网页搜索
和 PDF 处理共用一把 Key，在 `deploy/local/.env` 中填写一次：

```dotenv
DEEPSEEK_API_KEY=你的实际Key
```

`.tmp/local-deploy/config.json` 的 `remote_engine.endpoint` / `model` 仍需设置为可用值；
网页搜索需 `web_search.enabled=true`。搜索可通过 `DEEPSEEK_SEARCH_API_KEY` 单独覆盖，
否则复用问答 Key。不要将真实 Key 提交 Git。

修改 .env 后需要重建容器环境，单纯 restart 不会加载新环境变量：

```bash
docker compose -f deploy/local/compose.yml up -d --no-deps --force-recreate backend web
```

若当前镜像还没有新代码，需要先按下方步骤构建后端和前端，再执行上述命令。

查询改写默认关闭；需要时配置 `rag.query_rewrite`，确认所填模型可用。

```bash
# 状态与日志（后端首次启动日志可能包含初始管理员密码，请勿公开）
docker compose -f deploy/local/compose.yml ps
docker compose -f deploy/local/compose.yml logs --tail=80 web
# 修改前端后重建
docker compose -f deploy/local/compose.yml up -d --build web
# 修改后端后重建
python3 deploy/local/prepare.py
docker compose -f deploy/local/compose.yml up -d --build backend
# 停止，保留所有数据
docker compose -f deploy/local/compose.yml down
```

不要用 `down -v`，除非明确要删除本地 MySQL 数据。此配置用于本机测试，所有宿主端口只绑定 `127.0.0.1`。

后端镜像现在还包含 Python 3、Poppler 和 `scripts/read_pdf.py`，供 `fetch_knowledge` 读取原 PDF 和渲染页面。运行 prepare.py 后重建镜像才能获得这些依赖。外部下载地址、签名配置及模型视觉开关见 [原生 MCP 文档](../../docs/native-mcp.md)。
