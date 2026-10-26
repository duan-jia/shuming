# 贡献指南

感谢你关注 shuming。请先阅读 [README](README.md)、[架构](ARCHITECTURE.md)
和[开发指南](docs/getting-started.md)。

## 开始之前

- 缺陷报告请提供版本/提交、复现步骤、预期与实际结果、已脱敏的日志。
- 新功能和较大的重构先讨论使用场景与边界；小修复可以直接提交 PR。
- 安全问题遵循 [SECURITY.md](SECURITY.md)，不要公开漏洞细节或真实凭据。
- 根许可证尚待维护者确定。公开可见不等于获得开源授权；在许可证确定前，
  请先与维护者确认代码贡献和使用的授权安排。本指南本身不授予代码许可，
  不构成 CLA 或版权转让。

## 分支与 PR

1. 从最新 `master` 创建短期分支；不要直接推送或强推 `master`。
2. 一项 PR 聚焦一个目的，说明行为变化、兼容性、测试和未验证事项。
3. 修复附带回归测试；接口、配置、迁移和用户行为变化同步更新文档。
4. 合并前更新到最新主干，满足必需检查并解决讨论；采用 squash merge。

不要提交运行配置、数据库转储、论文全文、用户数据、密钥、模型权重、构建镜像
或服务器日志。新增依赖、字体、图片与测试数据要标明来源和授权；未知授权不应
视作允许再分发。涉及数据库或数据卷时必须说明备份与回滚，不自动删除既有数据。

## 验证

以 [CI](.github/workflows/ci.yml) 中的完整命令为准；根据变更范围选择本地验证，
并在 PR 中明确未执行的部分：

```bash
# 后端构建；依赖、测试环境及完整目标见开发指南和 CI
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF -DGGML_NATIVE=OFF
cmake --build build --target fast-llm-reasoner fast-llm-cli --parallel 2

# 兼容 MCP gateway
npm ci --prefix mcp-gateway
npm run typecheck --prefix mcp-gateway
npm test --prefix mcp-gateway
npm run build --prefix mcp-gateway

# 前端
npm ci --prefix web
npm run lint --prefix web
npm run build --prefix web
```

当前普通 CI 使用 GitHub-hosted 环境，但同仓库 PR 仍可能触发旧 Staging
工作流访问实验室服务器。公开前必须按[发布清单](docs/open-source-readiness.md)
解除该依赖并移走持久化 runner；本分支尚未修改工作流。硬件推理、真实模型效果与生产验收需要
单独记录环境与结果；构建通过不能代替这些验证。
