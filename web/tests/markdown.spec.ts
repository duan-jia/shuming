import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import ReactMarkdown from 'react-markdown';
import { markdownPlugins, markdownRehypePlugins } from '../src/lib/markdown.ts';

function render(content: string) {
  return renderToStaticMarkup(createElement(ReactMarkdown, { remarkPlugins: markdownPlugins, rehypePlugins: markdownRehypePlugins }, content));
}

test('inline sums render as math beside Chinese text', () => {
  const formula = String.raw`\sum_{t}\sum_{e\in L}\sum_o w_{\kappa(e,t),o} \le R,b(L)`;
  const html = render(`约束为$${formula}$，满足容量限制。`);
  assert.match(html, /class="katex"/);
  assert.match(html, /<msub>/);
  assert.match(html, /∑/);
  assert.match(html, /≤/);
  assert.ok(!html.includes('katex-error'));
  assert.ok(!html.includes('katex-display'));
  assert.ok(html.endsWith('，满足容量限制。</p>'));
});

test('display math renders with a display wrapper', () => {
  const html = render(String.raw`$$
\sum_{t} x_t \le R
$$`);
  assert.match(html, /class="katex-display"/);
  assert.ok(!html.includes('katex-error'));
});

test('math inside code and escaped dollar signs stay literal', () => {
  assert.equal(render('`$x_i$`'), '<p><code>$x_i$</code></p>');
  assert.equal(render('```tex\n$x_i$\n```'), '<pre><code class="language-tex">$x_i$\n</code></pre>');
  assert.equal(render('\\$x\\$'), '<p>$x$</p>');
});

test('invalid and incomplete formulas do not break the answer', () => {
  assert.match(render(String.raw`$\unknowncommand{x}$ 后续文字`), /后续文字/);
  assert.equal(render('未完成 $x'), '<p>未完成 $x</p>');
});

test('quoted emphasis renders beside Chinese text without adding spaces', () => {
  const quote = '“load balancing does not by itself solve the problem of overall congestion; it merely distributes it evenly”';
  assert.equal(render(`原文是**${quote}**。`), `<p>原文是<strong>${quote}</strong>。</p>`);
  assert.equal(render('这是**“中文引文”**的解释'), '<p>这是<strong>“中文引文”</strong>的解释</p>');
  assert.equal(render('这是**关键结论**的解释'), '<p>这是<strong>关键结论</strong>的解释</p>');
});

test('literal Markdown in inline code, fenced code and escaped text stays literal', () => {
  assert.equal(render('`**“引文”**`'), '<p><code>**“引文”**</code></p>');
  assert.equal(render('```text\n**“引文”**\n```'), '<pre><code class="language-text">**“引文”**\n</code></pre>');
  assert.equal(render('\\*\\*“引文”\\*\\*'), '<p>**“引文”**</p>');
});

test('nested emphasis and existing GFM features remain available', () => {
  assert.match(render('**重点 *说明* 内容**'), /<strong>重点 <em>说明<\/em> 内容<\/strong>/);
  assert.match(render('~~removed~~'), /<del>removed<\/del>/);
  assert.match(render('| 标题 |\n| --- |\n| **内容** |'), /<td><strong>内容<\/strong><\/td>/);
  assert.match(render('- [x] 完成'), /<input[^>]*type="checkbox"[^>]*checked=""/);
});
