# 网页 Agent 静态富内容

在支持工具调用的模型和已启用的 Agent 网页问答中，可以说：
“请用 HTML 卡片和表格展示对比，同时保留文字结论。”

Agent 获得一个仅限网页聊天运行时的工具（不新增公共 MCP 工具）：

```json
{
  "name": "display_html",
  "arguments": {
    "title": "方案对比",
    "html": "<section style=\"padding:12px;background:#f1f5f9\"><h2>吞吐对比</h2><table><tr><th>方案</th><th>吞吐</th></tr><tr><td>A</td><td>42</td></tr><tr><td>B</td><td>31</td></tr></table></section>",
    "summary": "演示数据：A 吞吐为 42，B 为 31。"
  }
}
```

## 行为与限制

- 必须提供非空 title、html、summary；分别最多 120、12000、2000 UTF-8 **字节**（中文占多个字节）。每个请求最多四个显示块。仍受现有工具调用、输出长度、上下文预算、超时和取消限制。
- HTML 仅支持基础文字、列表、表格和内联样式。不支持脚本、交互回传、按钮、表单、链接、图片、SVG、媒体、外部 CSS/字体或嵌入网页；不构成通用页面构建平台。图表请用表格/文字表达。
- 标题与摘要始终由 React 作为普通文字显示；关键结论也应保留在普通回答中。展示不代表新增检索证据。
- 块固定 320px 高，长内容在框内滚动，不通过脚本自动测高。不加载网页字体；中文等文字依赖设备本地字体（测试容器需安装 CJK 字体）。不接收 postMessage，不向框内传递令牌、Cookie 或父页面状态。
- 工具参数/预算错误使用既有失败工具状态；被过滤为空、浏览器不支持过滤或框加载错误时显示摘要和错误提示。
- 成功结果先写入已有工具日志，再经 SSE 的 `html_display` 事件送到聊天。历史接口按已有 request_id 关联成功的 tool_end 事件；只在所属用户/租户会话内返回。不新增数据库表/列或配置；需已有 Agent 日志迁移。日志不可读时显示降级提示。没有成功结果的旧消息不会生成富内容。
- 历史读取重用现有消息列表（最近 200 条）并限制每请求四块；被中断且未写入普通 assistant 消息的请求仍可在原有 trace 中审计，不额外合成聊天消息。

## 安全边界

Agent HTML 始终不可信。服务端只校验形状与大小，**不宣称服务端已净化**。前端在 DOMPurify 的严格标签/属性白名单中过滤片段，移除导航和资源属性，再放入受控 srcdoc 文档：

1. `sandbox=""`，不给 allow-scripts、allow-same-origin、表单、弹窗、下载或顶层导航权限；内容为 opaque origin，不能访问父页 DOM/凭证。
2. 受控文档先声明 CSP：default/script/connect/img/font/media/object/frame 均 none，base-uri/form-action none；只允许框内内联样式。生成内容不能替换 CSP，meta/base/style 等标签均不在白名单。
3. 只在 iframe 内使用 HTML，绝不把生成片段作为主页面 dangerouslySetInnerHTML。任意框内 CSS 不影响主页面；CSS 资源请求受 CSP 阻止。
4. 不依赖浏览器尚未普遍支持的 navigate-to CSP：导航标签和 URL 属性被移除，sandbox 另行禁止顶层导航。

仍需维护 DOMPurify/浏览器补丁。大小与数量限制不能完全消除恶意复杂布局的渲染成本。浏览器扩展、浏览器漏洞和服务端/模型本身泄密不在此边界内；不要把秘密写进生成内容。

## 自检

```sh
# 具备项目原生依赖的环境
cmake -S . -B build -DGGML_CUDA=OFF -DFASTLLM_BUILD_TESTS=ON
cmake --build build --target html-display-test
./build/bin/html-display-test

cd web
npm ci
npm run lint
npm run build
npm start
# 另一个终端；Node >=22.18 + Chromium，不需要真实后端/凭证
node --experimental-strip-types --test tests/html-display.spec.ts
```

浏览器测试使用真实聊天组件 + mock API/SSE，覆盖展示、历史回放、空内容降级、恶意标签/CSS 网络尝试、opaque-origin 隔离和 1280/768/390px 页面溢出。截图在 `.artifacts/html-display/`。这不等于真实模型、MySQL 和线上后端联调通过。
