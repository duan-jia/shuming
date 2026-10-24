# 开发＋评测：服务器仅部署 MySQL，前后端在本机

本配置只启动一个 MySQL 容器，日常开发和 Agent/RAG 评测共用。前后端在开发机运行；GitHub CI staging 与生产环境保持原样。**不新增或修改 workflow，不使用 runner 部署，不需要给 App 增加权限。部署由操作者手动完成。**

## 隔离资源

| 项目 | 开发＋评测数据库 | 现有 CI staging |
|---|---|---|
| Compose 项目 | `shuming-dev-eval` | `fast-llm-reasoner-ci-staging` |
| 配置目录 | `/srv/shuming-dev-eval` | `/srv/fastllm-staging` |
| 数据卷 | `shuming-dev-eval_mysql-data` | `fast-llm-reasoner-ci-staging_mysql-data` |
| 网络 | `shuming-dev-eval_network` | staging 自己的网络 |
| MySQL 宿主端口 | **仅** `127.0.0.1:23306` | 不映射宿主端口 |
| 生命周期 | 日常常驻，可手动停止 | CI 测试后停止，数据保留 |

两边的数据库都可以叫 `llm_platform`，但属于不同容器和卷，数据不互通。生产环境的历史项目名含有 staging；不要移动、删除、导入或重用其卷。本配置不复制任何现有数据库。

## 服务器手动部署

要求 Linux、Docker Compose v2（支持 `up --wait`）、Node.js >=22，以及本机 Docker 权限。先确认服务器可额外提供约 512 MiB 容器内存和足够磁盘空间。MySQL 使用 8.4 系列镜像；大版本升级必须另行备份与评审。

在服务器上取得本分支/PR 的代码，然后从仓库根目录运行。操作者应先将 `/srv/shuming-dev-eval` 创建为自己的目录（0700）；不要更改 CI/生产目录的权限。

```sh
node deploy/dev-eval/manage.mjs prepare
node deploy/dev-eval/manage.mjs check
node deploy/dev-eval/manage.mjs up
node deploy/dev-eval/manage.mjs status
# 需要暂停时，仅停止这个项目；数据保留
node deploy/dev-eval/manage.mjs stop
```

首次 up 会拉取缺失的 `mysql:8.4` 镜像；如服务器有经过验证的镜像源，请在部署前审阅并调整本配置的 image，不要修改现有生产/CI配置。

prepare 自动生成独立的 root/应用数据库密码，写入 `/srv/shuming-dev-eval/.env`（0600），不打印密码；公开 SQL 初始化快照写入 `init.sql`（0644，供容器内 MySQL 用户读取）。应用数据库用户为 `shuming`，不是 root，权限仅针对新实例的 `llm_platform`。

重复 prepare 不覆盖现有凭据。发现数据库卷已存在但凭据丢失、初始化文件不成对、符号链接或不安全权限时会拒绝操作。请恢复原配置，不要通过生成新密码来“重置”旧卷。

**不要运行 `down -v`、删除数据卷或提交 `.env`。** 备份数据库时使用操作者已有的受控备份方式；本变更不执行备份、迁移或清空数据。

## 开发机连接

服务器端口只监听回环地址，不能直接从公网连接，也不需要域名。通过已有 SSH 访问或 VPN/私网等已授权通道接入，不要把 23306/3306 开放给全网。

例如，在开发机使用已配置好的 SSH 主机别名建立端口转发（尖括号内容替换为现有别名）：

```sh
ssh -N -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:23306:127.0.0.1:23306 <existing-server-alias>
```

如果后端直接在开发机运行，其 MySQL 地址为 `127.0.0.1:23306`、数据库 `llm_platform`、用户 `shuming`。密码由操作者通过既有安全途径从服务器配置到开发机，不放命令行参数、聊天、Git 或测试日志中。

**后端在 Docker 容器里时，容器内的 localhost 不是开发机的 localhost。** 上面的宿主机回环隧道不能直接填写成容器内 `127.0.0.1:23306`；也不能想当然用 host.docker.internal 访问仅绑定回环的监听。可选择：

- 将 SSH 隧道运行在与本地后端相同的 Compose 网络内，通过隧道服务名连接。仅在容器网络内监听，不向宿主公网发布隧道端口；使用已有 SSH 身份/主机校验配置，不把私钥写入镜像或仓库。
- Linux 开发机可另行配置后端使用 host 网络，连接宿主机回环隧道；需审查后端自身监听端口的暴露范围。

本 PR 不替你选择操作系统相关的隧道/前后端网络配置。请根据开发机环境接好通道后，再改本地后端配置的 mysql.host/port/password。不要让前端直接连接 MySQL。原有 `deploy/local/prepare.py` 默认启动本地 MySQL，**不能原封不动同时用于这个远程数据库方案**；若复用其产物，请只运行本地前后端服务并调整后端连接，避免再启动或误连本机数据库。

## RAG 和数据集

RAG 索引、论文文件、embedding 模型保存在开发机持久目录，与本地后端配置一致；这次只部署 MySQL，并不自动将这些文件放到服务器。重建本地容器时保留这些目录，或按固定版本从论文重新构建索引。

公开论文内容经本地后端的认证接口导入；测试问题、相关性标注、预期工具调用和结果单独保存，不作为知识库正文索引。正式评测记录数据集版本、论文 SHA256、代码版本及索引/模型配置，评测过程中暂停修改语料。

## 验证与交付边界

```sh
node --check deploy/dev-eval/manage.mjs
node --test tests/dev_eval_deploy.test.mjs
```

上述检查覆盖单数据库服务、loopback 端口、CI/生产隔离、凭据、命令限制和恢复保护，不代替真实 Docker/MySQL 联调。作者未执行服务器部署；操作者后续应验证容器健康、本地后端登录/建会话/文档入库检索，并确认 CI/生产资源未变化。
