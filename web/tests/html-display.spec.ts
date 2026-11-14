// Run against `npm run dev` or `npm start` with Node >=22.18 and Chromium:
// node --experimental-strip-types --test tests/html-display.spec.ts
// TEST_URL and CHROMIUM_PATH optionally override the defaults. No backend needed.
// CJK_FONT_FAMILY optionally asserts the actual platform font for the Chinese heading.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

test('HTML display streams, replays and isolates hostile content', { timeout: 90000 }, async () => {
  const profile = await mkdtemp(join(tmpdir(), 'shuming-chat-'));
  const browser = spawn(process.env.CHROMIUM_PATH || '/usr/bin/chromium', [
    '--headless', '--no-sandbox', '--disable-dev-shm-usage', '--no-proxy-server',
    '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank',
  ]);
  let socket: WebSocket | undefined;
  try {
    const endpoint = await new Promise<string>((resolve, reject) => {
      let output = '';
      const timeout = setTimeout(() => reject(new Error('Chromium startup timed out')), 15000);
      browser.once('error', reject);
      browser.stderr.on('data', (chunk) => {
        output += chunk;
        const match = output.match(/DevTools listening on (ws:\/\/[^\s]+)/);
        if (match) { clearTimeout(timeout); resolve(match[1]); }
      });
    });
    const origin = `http://${new URL(endpoint).host}`;
    const target = await (await fetch(`${origin}/json/new?about:blank`, { method: 'PUT' })).json();
    socket = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise<void>((resolve, reject) => {
      socket!.addEventListener('open', () => resolve(), { once: true });
      socket!.addEventListener('error', reject, { once: true });
    });
    let sequence = 0;
    const pending = new Map<number, { resolve: (value: Record<string, unknown>) => void; reject: (error: Error) => void }>();
    const errors: string[] = [];
    const responses: string[] = [];
    const iframeSessions = new Map<string,string>();
    let cspBlocked = 0;
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data));
      if (message.method === 'Target.attachedToTarget') {
        const session = message.params.sessionId as string;
        if (message.params.targetInfo.type === 'iframe') iframeSessions.set(message.params.targetInfo.targetId, session);
        // Capture resource/CSP events from OOPIFs before their document runs.
        void command('Network.enable', {}, session)
          .then(() => command('Log.enable', {}, session))
          .then(() => command('Runtime.enable', {}, session))
          .then(() => command('Runtime.runIfWaitingForDebugger', {}, session))
          .catch(error => errors.push(String(error)));
      }
      if (message.method === 'Network.loadingFailed' && message.params.blockedReason === 'csp') ++cspBlocked;
      if (message.method === 'Log.entryAdded' &&
          /display-test\.invalid\/css/.test(message.params.entry.text) &&
          /content security policy/i.test(message.params.entry.text)) ++cspBlocked;
      if (message.method === 'Network.responseReceived') responses.push(message.params.response.url);
      if (message.method === 'Runtime.exceptionThrown') errors.push(JSON.stringify(message.params));
      const request = pending.get(message.id);
      if (request) {
        pending.delete(message.id);
        if (message.error) request.reject(new Error(JSON.stringify(message.error)));
        else request.resolve(message.result);
      }
    });
    const command = (method: string, params: Record<string, unknown> = {}, sessionId?: string): Promise<Record<string, unknown>> => {
      const id = ++sequence;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        socket!.send(JSON.stringify({ id, method, params, ...(sessionId ? {sessionId} : {}) }));
      });
    };
    const evaluate = async <T,>(expression: string): Promise<T> => {
      const result = await command('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
      assert.equal(result.exceptionDetails, undefined, JSON.stringify(result.exceptionDetails));
      return (result.result as { value: T }).value;
    };
    const wait = async (condition: string): Promise<void> => {
      for (let i = 0; i < 100; i++) {
        if (await evaluate<boolean>(condition)) return;
        await new Promise((resolve) => setTimeout(resolve, 50));
      }
      throw new Error(`Condition timed out: ${condition}; page=${await evaluate("document.body.innerText")}; errors=${JSON.stringify(errors)}`);
    };
    const click = async (label: string): Promise<void> => {
      await evaluate(`Array.from(document.querySelectorAll('button,aside span')).find(e=>e.textContent.trim()===${JSON.stringify(label)}).click()`);
    };
    const send = async (text: string): Promise<void> => {
      await evaluate(`(() => {
        const input = document.querySelector('textarea[aria-label="输入问题"]');
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value').set.call(input,${JSON.stringify(text)});
        input.dispatchEvent(new Event('input',{bubbles:true}));
      })()`);
      await wait(`Array.from(document.querySelectorAll('button')).some(b=>b.textContent.trim()==='发送'&&!b.disabled)`);
      await click('发送');
    };
    await command('Page.enable');
    await command('Network.enable');
    await command('Log.enable');
    await command('Runtime.enable');
    await command('DOM.enable');
    await command('CSS.enable');
    await command('Target.setAutoAttach', {autoAttach:true,waitForDebuggerOnStart:true,flatten:true});
    await command('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await command('Page.addScriptToEvaluateOnNewDocument', { source: `
      (() => {
        const original = window.fetch.bind(window);
        const sessions = ['A','B','C'].map(id=>({id,title:'会话'+id}));
        window.mock = { streams:{}, histories:{}, delayed:{}, status:200, created:[], failCreate:false };
        const json = data => new Response(JSON.stringify(data),{headers:{'Content-Type':'application/json'}});
        const now = Math.floor(Date.now()/1000);
        const auth = {token:'test.'+btoa(JSON.stringify({iat:now,exp:now+3600}))+'.only',user:{id:1,username:'test',role:'user',tenant_id:1}};
        let authenticated = false;
        window.mock.emit = (id,data) => window.mock.streams[id].enqueue(new TextEncoder().encode('data: '+JSON.stringify(data)+'\\n\\n'));
        window.fetch = async (input, options={}) => {
          const url = new URL(String(input), location.href);
          if (url.pathname === '/api/auth/config') return json({guest_enabled:false});
          if (url.pathname === '/api/auth/login') { authenticated=true; return json(auth); }
          if (url.pathname === '/api/auth/refresh') return authenticated ? json(auth) : new Response('{}',{status:401});
          if (url.pathname === '/api/auth/logout') { authenticated=false; return json({}); }
          if (url.pathname === '/api/sessions' && options.method === 'POST') {
            if (window.mock.failCreate) return new Response('{}',{status:503});
            const id = 'N'+sessions.length;
            const title = JSON.parse(options.body).title;
            window.mock.created.push({id,title}); sessions.push({id,title}); return json({id});
          }
          if (url.pathname === '/api/sessions') return json({sessions});
          if (url.pathname === '/api/chat/stream') {
            if (window.mock.status!==200) return new Response('failed',{status:window.mock.status});
            const id = JSON.parse(options.body).session_id;
            return new Response(new ReadableStream({start(controller){
              window.mock.streams[id]=controller;
              options.signal?.addEventListener('abort',()=>controller.error(new DOMException('aborted','AbortError')));
            }}),{headers:{'Content-Type':'text/event-stream'}});
          }
          if (url.pathname.startsWith('/api/sessions/')) {
            const id=url.pathname.split('/').pop();
            if (options.method==='DELETE') return json({});
            if (window.mock.delayed[id]) return new Promise(resolve=>window.mock.histories[id]=()=>resolve(json({messages:[{id:1,role:'assistant',content:'旧历史'+id}]})));
            return json({messages:window.mock.saved || []});
          }
          return original(input,options);
        };
      })();` });
    await command('Page.navigate', { url: process.env.TEST_URL || 'http://localhost:3000' });
    await wait(`document.querySelector('input[type="password"]')!==null`);
    await wait(`Object.keys(document.querySelector('input')).some(k=>k.startsWith('__reactProps'))`);
    await evaluate(`(() => { const input = document.querySelector('input[autocomplete="username"]'); Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input,'test'); input.dispatchEvent(new Event('input',{bubbles:true})); })()`);
    await evaluate(`(() => { const input = document.querySelector('input[type="password"]'); Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input,'test-password'); input.dispatchEvent(new Event('input',{bubbles:true})); })()`);
    await click('登录');
    await wait(`document.body.innerText.includes('会话A')`);

    await click('会话A');
    await send('请用 HTML 展示实验结果卡片和对比表');
    await wait("!!window.mock.streams.A");
    const block = {type:"html_display", title:"实验结果 · Comparison",
      summary:"方案 A 吞吐 42，方案 B 吞吐 31；仅供演示，非检索证据。",
      html:'<section style="padding:12px;background:#f1f5f9;border-radius:12px"><h2>实验结果</h2><p>吞吐对比 / Throughput</p><table><caption>两种方案</caption><tr><th scope="col">方案</th><th scope="col">吞吐</th></tr><tr><td>A</td><td style="color:#047857">42</td></tr><tr><td>B</td><td>31</td></tr></table></section>'};
    await evaluate(`window.mock.emit('A',{type:'agent_turn',turn:1});window.mock.emit('A',{type:'html_display',display:${JSON.stringify(block)}})`);
    await wait("document.querySelector('.html-display iframe')?.srcdoc.includes('Throughput')");
    assert.equal(await evaluate("document.querySelector('.html-display iframe').getAttribute('sandbox')"),"");
    assert.equal(await evaluate("document.querySelector('.html-display iframe').contentDocument"),null,"Opaque sandbox origin");
    await evaluate("window.mock.emit('A',{type:'answer',content:'方案 A 吞吐为 42，方案 B 为 31。'});window.mock.emit('A',{type:'done'});window.mock.streams.A.close()");
    await wait("document.querySelector('.composer-send').textContent==='发送'");
    await mkdir('../.artifacts/html-display', {recursive:true});
    for (const width of [1280,768,390]) {
      await command('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:width===390});
      await new Promise(resolve=>setTimeout(resolve,250));
      assert.equal(await evaluate("document.documentElement.scrollWidth<=window.innerWidth"),true, `No host overflow at ${width}`);
      const shot = await command('Page.captureScreenshot',{format:'png'});
      await writeFile(`../.artifacts/html-display/desktop-${width}.png`, Buffer.from(String(shot.data),'base64'));
    }
    // Replay uses exactly the same renderer as live events.
    await evaluate(`window.mock.saved=[{id:20,role:'assistant',content:'历史回答',displays:[${JSON.stringify(block)}]}]`);
    await click('会话B');
    await wait("document.body.innerText.includes('历史回答')&&!!document.querySelector('.html-display iframe')");
    // Sandboxed srcdoc may be an out-of-process iframe in Chromium. Inspect
    // it through its own CDP session without weakening browser isolation.
    let child: string | undefined;
    let iframeSession: string | undefined;
    for (let attempt = 0; attempt < 100; attempt++) {
      const tree = await command('Page.getFrameTree');
      child = (tree.frameTree as {childFrames?: {frame: {id:string}}[]}).childFrames?.[0]?.frame.id;
      if (child) break;
      const targets = await command('Target.getTargets');
      const iframe = (targets.targetInfos as {type:string;targetId:string}[]).find(target => target.type === 'iframe');
      if (iframe) {
        iframeSession = iframeSessions.get(iframe.targetId);
        if (!iframeSession) {
          const attached = await command('Target.attachToTarget', {targetId:iframe.targetId,flatten:true});
          iframeSession = attached.sessionId as string;
        }
        await command('Page.enable', {}, iframeSession);
        await command('Runtime.enable', {}, iframeSession);
        await command('DOM.enable', {}, iframeSession);
        await command('CSS.enable', {}, iframeSession);
        const iframeTree = await command('Page.getFrameTree', {}, iframeSession);
        child = (iframeTree.frameTree as {frame:{id:string}}).frame.id;
        break;
      }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    assert.ok(child, 'Replay iframe child document attached');
    const frameCommand = (method:string, params:Record<string,unknown> = {}) => command(method, params, iframeSession);
    const world = await frameCommand('Page.createIsolatedWorld',{frameId:child,worldName:'display-inspection'});
    const inside = await frameCommand('Runtime.evaluate', {contextId:world.executionContextId,
      expression:"({text:document.body.innerText, navigation:document.querySelectorAll('a,form,button,iframe,script,img').length})",returnByValue:true});
    const rendered = (inside.result as {value:{text:string,navigation:number}}).value;
    assert.ok(rendered.text.includes('实验结果') && rendered.text.includes('Throughput') && rendered.text.includes('42'));
    assert.equal(rendered.navigation,0);
    // Text presence alone does not detect missing CJK glyphs (tofu). Inspect
    // Chromium's actual font usage inside the opaque-origin iframe via CDP.
    const heading = await frameCommand('Runtime.evaluate', {contextId:world.executionContextId,
      expression:"document.querySelector('h2')"});
    await frameCommand('DOM.getDocument');
    const node = await frameCommand('DOM.requestNode', {objectId:(heading.result as {objectId:string}).objectId});
    const fonts = await frameCommand('CSS.getPlatformFontsForNode', {nodeId:node.nodeId});
    const usedFonts = fonts.fonts as {familyName:string;glyphCount:number}[];
    console.info('Iframe Chinese heading platform fonts:', JSON.stringify(usedFonts));
    assert.ok(usedFonts.some(font => font.glyphCount > 0), 'Chinese heading uses a platform font');
    if (process.env.CJK_FONT_FAMILY) {
      assert.ok(usedFonts.some(font => font.familyName === process.env.CJK_FONT_FAMILY && font.glyphCount >= 4),
        `Expected all four Chinese glyphs from ${process.env.CJK_FONT_FAMILY}`);
    }
    // Hostile tags/attributes are removed; inline CSS is isolated and its network blocked.
    const evil = {...block,title:"安全边界测试",html:
      '<p style="background:url(https://display-test.invalid/css)">Visible safe text</p>' +
      '<script>parent.__attacked=true;fetch("https://display-test.invalid/script")</script>' +
      '<img src="https://display-test.invalid/image" onerror="parent.__attacked=true">' +
      '<a href="https://display-test.invalid/nav" target="_top">link text</a>' +
      '<meta http-equiv="refresh" content="0;url=https://display-test.invalid/refresh">' +
      '<base href="https://display-test.invalid/"><iframe src="https://display-test.invalid/frame"></iframe>' +
      '<form action="https://display-test.invalid/form"><button>Submit</button></form>' +
      '<svg onload="parent.__attacked=true"><a href="https://display-test.invalid/svg">svg</a></svg>'};
    await evaluate(`window.mock.saved=[{id:21,role:'assistant',content:'安全回答',displays:[${JSON.stringify(evil)}]}]`);
    await click('会话C');
    await wait("document.querySelector('.html-display iframe')?.title==='安全边界测试'");
    const srcdoc = await evaluate<string>("document.querySelector('.html-display iframe').srcdoc");
    assert.ok(srcdoc.includes("default-src 'none'"));
    assert.ok(!/<(script|img|a |meta http-equiv="refresh"|base |iframe|form|button|svg)/i.test(srcdoc));
    assert.ok(!/onerror|onload|target=|href=|src=/i.test(srcdoc));
    await new Promise(resolve=>setTimeout(resolve,500));
    assert.equal(await evaluate("window.__attacked===undefined"),true);
    assert.ok(cspBlocked > 0, "CSS resource attempt blocked by iframe CSP");
    assert.ok(!responses.some(url=>url.includes('display-test.invalid')),"No hostile resource response");
    assert.equal(await evaluate("location.hostname"),"localhost");
    // Filtered-empty payload degrades to persistent readable summary.
    const empty = {...block,html:'<script>alert(1)</script>'};
    await evaluate(`window.mock.saved=[{id:22,role:'assistant',content:'文字保留',displays:[${JSON.stringify(empty)}]}]`);
    await click('会话A');
    await wait("document.body.innerText.includes('富内容无法显示')");
    assert.equal(await evaluate("document.querySelectorAll('.html-display iframe').length"),0);
    assert.ok(await evaluate("document.body.innerText.includes('方案 A 吞吐 42')"));
    assert.deepEqual(errors,[]);
  } finally {
    socket?.close();
    browser.kill();
    await new Promise<void>((resolve) => {
      if (browser.exitCode !== null) resolve(); else browser.once('exit', () => resolve());
    });
    await rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
});
