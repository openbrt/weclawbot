import assert from "node:assert/strict";
import fs from "node:fs/promises";

import { renderScreenDocumentPreviewPages, previewSummary, writeScreenDocumentPreviewFiles } from "../lib/screen-preview.mjs";

const width = 16;
const height = 8;
const stride = 2;
const bytes = Buffer.alloc(stride * height, 0x00);
for (let i = 0; i < Math.min(width, height); i += 1) {
  bytes[i * stride + (i >> 3)] |= 0x80 >> (i & 7);
}

const document = {
  schema: "weclawbot.screen_document.v1",
  pages: [{
    format: "mono1",
    width,
    height,
    stride,
    data_b64: bytes.toString("base64"),
  }],
};

const preview = renderScreenDocumentPreviewPages(document, { scale: 2 });

assert.equal(preview.length, 1);
assert.equal(preview[0].width, width * 2);
assert.equal(preview[0].height, height * 2);
assert.equal(preview[0].png.subarray(0, 8).toString("hex"), "89504e470d0a1a0a");
assert.ok(preview[0].bytes > 50);
assert.match(preview[0].sha256, /^[0-9a-f]{64}$/u);

const summary = previewSummary(preview);
assert.equal(summary.available, true);
assert.equal(summary.pages[0].mime_type, "image/png");

const files = await writeScreenDocumentPreviewFiles(document, { scale: 2 });
assert.equal(files.available, true);
assert.equal(files.pages.length, 1);
assert.match(files.pages[0].path, /\.png$/u);
const persisted = await fs.readFile(files.pages[0].path);
assert.equal(persisted.subarray(0, 8).toString("hex"), "89504e470d0a1a0a");
await fs.rm(files.output_dir, { recursive: true, force: true });

console.log("screen-preview renderer: ok");
