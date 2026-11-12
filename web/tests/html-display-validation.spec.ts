import { test } from "node:test";
import assert from "node:assert/strict";
import { normalizeDisplays, DISPLAY_CSP } from "../src/lib/html-display.ts";

test("reject malformed/oversized displays and bound history blocks", () => {
  const valid = { type: "html_display", title: "中文", html: "<p>42</p>", summary: "42" };
  assert.deepEqual(normalizeDisplays([valid]), [valid]);
  for (const item of [null, {}, { ...valid, type: "page" }, { ...valid, html: "" },
    { ...valid, title: "中".repeat(41) }, { ...valid, html: "中".repeat(4001) },
    { ...valid, summary: 123 }, { ...valid, summary: " ".repeat(10) }]) {
    assert.deepEqual(normalizeDisplays([item]), []);
  }
  assert.deepEqual(normalizeDisplays("not an array"), []);
  assert.equal(normalizeDisplays(Array(20).fill(valid)).length, 4);
  for (const directive of ["default-src 'none'", "script-src 'none'", "connect-src 'none'",
    "base-uri 'none'", "form-action 'none'"]) assert.ok(DISPLAY_CSP.includes(directive));
});
