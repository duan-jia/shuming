# 本地 Docker 测试

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

默认可测试登录、会话、Markdown 文档入库和真实 BGE 检索。生成问答与 PDF 论文卡片还需要在 `.tmp/local-deploy/config.json` 的 `remote_engine` 中填写可用的 endpoint、model、api_key，然后执行：

```bash
docker compose -f deploy/local/compose.yml restart backend web
```

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
