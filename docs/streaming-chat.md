# 流式对话与前端交互

分支：`feat/streaming-chat-ui`。无需数据库迁移或新增依赖，继续使用现有 C++ SSE 接口。

## 改动

- Next.js `/api/chat/stream` 显式透传上游 `ReadableStream`，不等待完整答案；保留 Authorization、请求 ID 与取消信号，禁止代理缓存和 Nginx 缓冲。
- 独立 SSE 增量解析器支持跨网络分片、UTF-8 中文/emoji、CRLF、无空格字段、多行 data 和注释心跳；错误、提前断流保留已有内容并提示。
- 石墨黑 / 钢蓝对话界面，响应式研究欢迎页与任务建议、用户头像、Markdown 回答、可折叠思考过程、工具与生成状态、复制回答。
- 多行输入：Enter 发送、Shift+Enter 换行；中文输入法选字不误发送。停止按钮保留部分回答。
- 只有滚动至底部附近时自动跟随，向上阅读不会被每个 token 拉回底部；保留后台会话独立生成行为。

## 验证

Node >=22.18、Chromium（默认 `/usr/bin/chromium`）：

```sh
cd web
npm ci
npm run lint
npm run build
node --experimental-strip-types --test tests/sse.spec.ts
# 终端 1，默认上游配置 http://127.0.0.1:9006
npm start
# 终端 2，9006 需空闲：启动测试上游，验证首个事件在上游结束前已到达
node --experimental-strip-types --test tests/stream-proxy.spec.ts
# 浏览器回归使用模拟登录、历史及 SSE，不需要真实后端
node --experimental-strip-types --test tests/session-switch.spec.ts
```

浏览器测试覆盖 1280 / 768 / 390px 无横向溢出、移动导航隐藏、分段内容完成前可见、思考事件、后台会话隔离、错误/提前断流、停止、输入法和退出清理；截图输出到 `/tmp/shuming-{desktop,tablet,mobile,session-switch}.png`。

## 部署注意

继续设置 `BACKEND_API_BASE` 指向 C++ 服务。外层反向代理须禁用 `/api/chat/stream` 的响应缓冲/压缩，并为模型推理设置足够的读取超时。此变更未部署，也未连接生产模型进行实测；自动化使用可控上游验证流式传输及前端交互。
