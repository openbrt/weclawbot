import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import {
  createPreviewArtifactDir,
  listRecentPreviewManifests,
  markPreviewManifest,
  writePreviewManifest,
} from "../lib/preview-manifest.mjs";
import { writeScreenDocumentPreviewFiles } from "../lib/screen-preview.mjs";

const root = await fs.mkdtemp(path.join(os.tmpdir(), "weclawbot-preview-manifest-test-"));
process.env.WECLAWBOT_PREVIEW_SPOOL_DIR = root;

const width = 8;
const height = 8;
const stride = 1;
const bytes = Buffer.alloc(stride * height, 0x00);
for (let y = 0; y < height; y += 1) bytes[y * stride] = 0x80;

const document = {
  schema: "weclawbot.screen_document.v1",
  id: "manifest-test",
  pages: [{
    format: "mono1",
    width,
    height,
    stride,
    data_b64: bytes.toString("base64"),
  }],
};

const artifactDir = await createPreviewArtifactDir(document.id);
assert.ok(artifactDir.startsWith(root));

const preview = await writeScreenDocumentPreviewFiles(document, { outputDir: artifactDir });
const manifest = await writePreviewManifest({
  document,
  preview,
  source: "test",
  status: { applied: true },
});

assert.ok(manifest.path.startsWith(root));
assert.equal(manifest.preview.pages.length, 1);

const recent = await listRecentPreviewManifests({ sinceMs: Date.now() - 10_000 });
assert.equal(recent.length, 1);
assert.equal(recent[0].id, manifest.id);

await markPreviewManifest(manifest.path, { delivered_at: new Date().toISOString() });
const afterDelivery = await listRecentPreviewManifests({ sinceMs: Date.now() - 10_000 });
assert.equal(afterDelivery.length, 0);

delete process.env.WECLAWBOT_PREVIEW_SPOOL_DIR;
await fs.rm(root, { recursive: true, force: true });

console.log("preview-manifest spool: ok");
