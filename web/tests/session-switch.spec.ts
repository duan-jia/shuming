// Run against `npm run dev` or `npm start` with Node >=22.18 and Chromium:
// node --experimental-strip-types --test tests/session-switch.spec.ts
// TEST_URL and CHROMIUM_PATH optionally override the defaults. No backend needed.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

test('background replies and session navigation stay isolated', { timeout: 90000 }, async () => {
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
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data));
      if (message.method === 'Runtime.exceptionThrown') errors.push(JSON.stringify(message.params));
      const request = pending.get(message.id);
      if (request) {
        pending.delete(message.id);
        if (message.error) request.reject(new Error(JSON.stringify(message.error)));
        else request.resolve(message.result);
      }
    });
    const command = (method: string, params: Record<string, unknown> = {}): Promise<Record<string, unknown>> => {
      const id = ++sequence;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        socket!.send(JSON.stringify({ id, method, params }));
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
    await command('Runtime.enable');
    await command('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await command('Page.addScriptToEvaluateOnNewDocument', { source: `
      (() => {
        const original = window.fetch.bind(window);
        const sessions = ['A','B','C'].map(id=>({id,title:'会话'+id}));
        window.mock = { streams:{}, histories:{}, delayed:{}, status:200 };
        const json = data => new Response(JSON.stringify(data),{headers:{'Content-Type':'application/json'}});
        window.mock.emit = (id,data) => window.mock.streams[id].enqueue(new TextEncoder().encode('data: '+JSON.stringify(data)+'\\n\\n'));
        window.fetch = async (input, options={}) => {
          const url = new URL(String(input), location.href);
          if (url.pathname === '/api/auth/login') return json({token:'test-only',user:{role:'user'}});
          if (url.pathname === '/api/sessions' && options.method === 'POST') {
            const id = 'N'+sessions.length; sessions.push({id,title:'新会话'+id}); return json({id});
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
            return json({messages:[]});
          }
          return original(input,options);
        };
      })();` });
    await command('Page.navigate', { url: process.env.TEST_URL || 'http://localhost:3000' });
    await wait(`document.querySelector('input[type="password"]')!==null`);
    await wait(`Object.keys(document.querySelector('input')).some(k=>k.startsWith('__reactProps'))`);
    await evaluate(`(() => { const input = document.querySelector('input[type="password"]'); Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(input,'test-password'); input.dispatchEvent(new Event('input',{bubbles:true})); })()`);
    await click('登录');
    await wait(`document.body.innerText.includes('会话A')`);
    for (const [name, width] of [['desktop',1280],['tablet',768],['mobile',390]] as const) {
      await command('Emulation.setDeviceMetricsOverride', { width, height: 900, deviceScaleFactor: 1, mobile: width < 768 });
      await new Promise(resolve => setTimeout(resolve, 350));
      assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true);
      if (width < 768) assert.equal(await evaluate('document.querySelector("aside").getBoundingClientRect().right <= 0'), true);
      const shot = await command('Page.captureScreenshot', { format: 'png' });
      await writeFile('/tmp/shuming-' + name + '.png', Buffer.from(shot.data as string, 'base64'));
    }
    await command('Emulation.setDeviceMetricsOverride', { width:1280, height:900, deviceScaleFactor:1, mobile:false });
    await click('会话A');
    await send('问题A');
    await wait(`!!window.mock.streams.A`);
    await evaluate(`window.mock.emit('A',{type:'reasoning',content:'先检索实验室资料，再对比研究结论。'});window.mock.emit('A',{type:'answer',content:'回答A第一段'})`);
    await wait(`document.body.innerText.includes('回答A第一段')`);
    assert.equal(await evaluate(`getComputedStyle(document.querySelector('.prose')).fontSize`), '19px');
    assert.ok(await evaluate<number>(`document.querySelector('main').getBoundingClientRect().width`) > 896);
    assert.equal(await evaluate(`document.querySelector('.message-avatar, .assistant-avatar-logo, .message-label, .stream-badge') === null`), true);
    assert.equal(await evaluate(`document.querySelector('header').innerText.trim()`), '');
    assert.equal(await evaluate(`getComputedStyle(document.querySelector('header')).display`), 'none');
    assert.equal(await evaluate(`document.querySelector('.composer-hint') === null`), true);
    assert.equal(await evaluate(`document.body.innerText.includes('Shift + Enter')`), false);
    await click('会话B');
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await send('问题B');
    await wait(`!!window.mock.streams.B`);
    await evaluate(`window.mock.emit('A',{type:'answer',content:'后台第二段'})`);
    assert.equal(await evaluate(`document.body.innerText.includes('后台第二段')`), false);
    await evaluate(`window.mock.emit('B',{type:'answer',content:'回答B'});window.mock.emit('B',{type:'done'})`);
    // Deliberately keep the transport open after done: UI must still unlock.
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled&&document.body.innerText.includes('回答B')`);
    await click('会话A');
    await wait(`document.body.innerText.includes('回答A第一段后台第二段')&&document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.emit('A',{type:'answer',content:'\\n\\n### 研究要点\\n\\n- 先确认研究问题与实验假设\\n- 对照文档中的方法、指标与局限\\n\\n> 结论应结合原始文献和实验条件核实。\\n\\n| 对比维度 | 阅读重点 |\\n| --- | --- |\\n| 方法 | 适用场景与前提 |\\n| 证据 | 数据集与评价指标 |'})`);
    await wait(`document.body.innerText.includes('研究要点')`);
    await command('Emulation.setDeviceMetricsOverride', { width:390, height:900, deviceScaleFactor:1, mobile:true });
    await new Promise(resolve => setTimeout(resolve, 350));
    assert.equal(await evaluate('document.documentElement.scrollWidth <= innerWidth'), true);
    assert.equal(await evaluate(`getComputedStyle(document.querySelector('.prose')).fontSize`), '18px');
    const mobileChat = await command('Page.captureScreenshot', { format:'png' });
    await writeFile('/tmp/shuming-mobile-chat.png', Buffer.from(mobileChat.data as string,'base64'));
    await command('Emulation.setDeviceMetricsOverride', { width:1280, height:900, deviceScaleFactor:1, mobile:false });
    await new Promise(resolve => setTimeout(resolve, 350));
    const screenshot = await command('Page.captureScreenshot', { format: 'png' });
    await writeFile(process.env.SCREENSHOT_PATH || '/tmp/shuming-session-switch.png', Buffer.from(screenshot.data as string, 'base64'));
    // Deliver tiny chunks across separate tasks, then finish with a pending tail.
    // The DOM should update in batches and preserve every byte at completion.
    await evaluate(`(() => {
      window.streamMutations = 0;
      window.streamObserver = new MutationObserver(() => window.streamMutations++);
      window.streamObserver.observe(document.querySelector('article.assistant .prose'), {subtree:true,childList:true,characterData:true});
    })()`);
    await evaluate(`(async () => {
      window.mock.emit('A',{type:'answer',content:'\\n\\n'});
      for (let i=0;i<200;i++) {
        window.mock.emit('A',{type:'answer',content:'流'});
        await new Promise(resolve=>setTimeout(resolve,2));
      }
      window.mock.emit('A',{type:'answer',content:'尾部完成'});
      window.mock.emit('A',{type:'done'});
    })()`);
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    assert.equal(await evaluate(`document.querySelector('article.assistant .prose').textContent.endsWith('流'.repeat(200)+'尾部完成')`), true);
    assert.ok(await evaluate<number>('window.streamMutations') < 80, 'Streaming should batch DOM updates');
    await evaluate('window.streamObserver.disconnect()');
    // A second question after a multi-screen answer must only scroll the chat pane.
    for (const width of [1280, 390]) {
      await command('Emulation.setDeviceMetricsOverride', { width, height:900, deviceScaleFactor:1, mobile:width < 768 });
      await send('长会话滚动检查');
      await wait(`!!window.mock.streams.A && document.querySelector('textarea').disabled`);
      await evaluate(`window.mock.emit('A',{type:'answer',content:('长回答：检查滚动边界。\\n\\n').repeat(100)})`);
      await wait(`document.querySelector('main').scrollHeight > document.querySelector('main').clientHeight * 2`);
      await new Promise(resolve=>setTimeout(resolve,150));
      console.log('long-chat geometry', width, await evaluate(`(() => { const m=document.querySelector('main'),c=document.querySelector('.composer-area').getBoundingClientRect();return {rootHeight:document.documentElement.scrollHeight,viewport:innerHeight,windowY:scrollY,mainTop:m.getBoundingClientRect().top,mainHeight:m.clientHeight,scroll:m.scrollTop,total:m.scrollHeight,composerBottom:c.bottom};})()`));
      assert.equal(await evaluate('document.documentElement.scrollHeight <= innerHeight'), true, 'Only the message pane should scroll');
      assert.equal(await evaluate('scrollY'), 0, 'Streaming must not move the document');
      assert.ok(await evaluate<number>(`Math.abs(document.querySelector('.composer-area').getBoundingClientRect().bottom-innerHeight)`) < 1);
      await wait(`(() => {const m=document.querySelector('main');return m.scrollHeight-m.clientHeight-m.scrollTop < 3})()`);
      await evaluate(`document.querySelector('main').scrollTop=200`);
      await new Promise(resolve=>setTimeout(resolve,100));
      await evaluate(`window.mock.emit('A',{type:'answer',content:'继续追加内容\\n\\n'.repeat(20)})`);
      await new Promise(resolve=>setTimeout(resolve,150));
      assert.ok(await evaluate<number>(`Math.abs(document.querySelector('main').scrollTop-200)`) < 3, 'Reading history must pause auto-follow');
      await click('查看最新回答 ↓');
      await wait(`(() => {const m=document.querySelector('main');return m.scrollHeight-m.clientHeight-m.scrollTop < 3})()`);
      const composerBefore = await evaluate<number>(`document.querySelector('.composer-area').getBoundingClientRect().top`);
      for (let i=0;i<5;i++) {
        await evaluate(`window.mock.emit('A',{type:'answer',content:'新一段持续输出。\\n\\n'.repeat(10)})`);
        await new Promise(resolve=>setTimeout(resolve,80));
        assert.equal(await evaluate('scrollY'), 0);
        assert.equal(await evaluate<number>(`document.querySelector('.composer-area').getBoundingClientRect().top`), composerBefore);
        await wait(`(() => {const m=document.querySelector('main');return m.scrollHeight-m.clientHeight-m.scrollTop < 3})()`);
      }
      const longShot = await command('Page.captureScreenshot', { format:'png' });
      await writeFile('/tmp/shuming-long-chat-' + width + '.png', Buffer.from(longShot.data as string,'base64'));
      await evaluate(`window.mock.emit('A',{type:'done'})`);
      await wait(`!document.querySelector('textarea').disabled`);
    }
    await command('Emulation.setDeviceMetricsOverride', { width:1280,height:900,deviceScaleFactor:1,mobile:false });
    await send('再次提问A');
    await wait(`document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.emit('A',{type:'error',error:'测试生成失败'})`);
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled&&document.body.innerText.includes('测试生成失败')`);
    // Delayed history must not overwrite a later generation or the active view.
    await evaluate(`window.mock.delayed.C=true`);
    await click('会话C');
    await wait(`!!window.mock.histories.C`);
    await click('会话B');
    await evaluate(`window.mock.histories.C()`);
    assert.equal(await evaluate(`document.body.innerText.includes('旧历史C')`), false);
    await click('会话C');
    await send('问题C');
    await wait(`!!window.mock.streams.C`);
    await evaluate(`window.mock.histories.C();window.mock.emit('C',{type:'answer',content:'新回答C'})`);
    await wait(`document.body.innerText.includes('新回答C')`);
    assert.equal(await evaluate(`document.body.innerText.includes('问题C')`), true);
    // Finishing a background reply must not clear another session's busy flag.
    await click('会话A');
    await send('并行A');
    await wait(`document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.emit('C',{type:'done'})`);
    assert.equal(await evaluate(`document.querySelector('textarea[aria-label="输入问题"]').disabled`), true);
    await evaluate(`window.mock.emit('A',{type:'done'})`);
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.status=503`);
    await send('请求失败');
    await wait(`document.body.innerText.includes('HTTP 503')&&!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.status=200`);
    // New-session navigation must not cancel the old stream.
    await send('保留后台');
    await wait(`document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await click('新建对话');
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.emit('A',{type:'answer',content:'仍在后台'})`);
    await click('会话A');
    await wait(`document.body.innerText.includes('仍在后台')`);
    await evaluate(`window.mock.streams.A.close()`);
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await send('DONE标记');
    await wait(`document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`window.mock.streams.A.enqueue(new TextEncoder().encode('data: [DONE]\\n\\n'))`);
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await send('停止测试');
    await wait(`document.querySelector('textarea').disabled`);
    await click('停止生成');
    await wait(`!document.querySelector('textarea').disabled && document.body.innerText.includes('已停止生成')`);
    await evaluate(`(() => {
      const input=document.querySelector('textarea');
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value').set.call(input,'中文输入中');
      input.dispatchEvent(new Event('input',{bubbles:true}));
    })()`);
    await evaluate(`document.querySelector('textarea').dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',isComposing:true,bubbles:true}))`);
    assert.equal(await evaluate(`document.querySelector('textarea').disabled`), false);
    await send('删除生成会话');
    await wait(`document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await evaluate(`Array.from(document.querySelectorAll('aside span')).find(e=>e.textContent==='会话A').parentElement.querySelector('button').click()`);
    await wait(`!document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await send('自动创建并生成');
    await wait(`document.querySelector('textarea[aria-label="输入问题"]').disabled`);
    await click('退出');
    await click('登录');
    await wait(`document.body.innerText.includes('会话A')`);
    assert.equal(await evaluate(`document.body.innerText.includes('仍在后台')`), false);
    assert.deepEqual(errors, []);
  } finally {
    socket?.close();
    browser.kill();
    await new Promise<void>((resolve) => {
      if (browser.exitCode !== null) resolve(); else browser.once('exit', () => resolve());
    });
    await rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
});
