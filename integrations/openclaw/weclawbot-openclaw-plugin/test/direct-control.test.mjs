import assert from "node:assert/strict";

import { resolveDeviceContext, validateScreenDocument } from "../lib/direct-control.mjs";

const context = resolveDeviceContext();
const viewport = context.content_viewport;
const bytes = Buffer.alloc(Math.ceil(viewport.width / 8) * viewport.height, 0x00);
bytes[0] = 0x80;
const valid = validateScreenDocument({
  schema: "weclawbot.screen_document.v1",
  id: "test-card",
  base_revision: "",
  expires_at: new Date(Date.now() + 60_000).toISOString(),
  target: "content",
  kind: "replace",
  pages: [{
    format: "mono1",
    width: viewport.width,
    height: viewport.height,
    stride: Math.ceil(viewport.width / 8),
    data_b64: bytes.toString("base64"),
  }],
}, context);
assert.equal(valid.ok, true, valid.errors.join("; "));
assert.equal(valid.direct_delivery_ready, false);
assert.equal(valid.warnings.some((warning) => warning.includes("high_ink_coverage")), false);

const highInkBytes = Buffer.alloc(Math.ceil(viewport.width / 8) * viewport.height, 0xff);
highInkBytes[0] = 0x00;
const highInk = validateScreenDocument({
  schema: "weclawbot.screen_document.v1",
  id: "inverted-card",
  base_revision: "",
  expires_at: new Date(Date.now() + 60_000).toISOString(),
  target: "content",
  kind: "replace",
  pages: [{
    format: "mono1",
    width: viewport.width,
    height: viewport.height,
    stride: Math.ceil(viewport.width / 8),
    data_b64: highInkBytes.toString("base64"),
  }],
}, context);
assert.equal(highInk.ok, true, highInk.errors.join("; "));
assert.ok(highInk.warnings.some((warning) => warning.includes("high_ink_coverage")));

const invalid = validateScreenDocument({
  ...{ schema: "weclawbot.screen_document.v1", id: "bad", base_revision: "" },
  expires_at: new Date(Date.now() + 60_000).toISOString(),
  target: "content",
  kind: "replace",
  pages: [{ format: "mono1", width: 369, height: 206, stride: 47, data_b64: "AA==" }],
}, context);
assert.equal(invalid.ok, false);
assert.ok(invalid.errors.some((error) => error.includes("width")));

const uniform = validateScreenDocument({
  schema: "weclawbot.screen_document.v1",
  id: "bad-clear",
  base_revision: "",
  expires_at: new Date(Date.now() + 60_000).toISOString(),
  target: "content",
  kind: "replace",
  pages: [{
    format: "mono1",
    width: viewport.width,
    height: viewport.height,
    stride: Math.ceil(viewport.width / 8),
    data_b64: Buffer.alloc(Math.ceil(viewport.width / 8) * viewport.height, 0x00).toString("base64"),
  }],
}, context);
assert.equal(uniform.ok, false);
assert.ok(uniform.errors.some((error) => error.includes("uniform_screen_document")));

console.log("direct-control validator: ok");
