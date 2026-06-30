import assert from "node:assert/strict";
import path from "node:path";

import { collectCommandStrings, extractScreenDocumentPathFromExecParams, normalizeDeliveryContext } from "../lib/openclaw-preview.mjs";

const extracted = extractScreenDocumentPathFromExecParams({
  command: "~/.npm-global/bin/weclawbotctl screen /tmp/weclawbot-musk.json --force --timeout 20",
  cwd: "/home/csc/.openclaw/workspace",
});

assert.equal(extracted, "/tmp/weclawbot-musk.json");

const nested = collectCommandStrings({
  payload: {
    args: ["weclawbotctl", "screen", "relative-doc.json", "--timeout", "20"],
  },
});

assert.ok(nested.some((value) => value.includes("weclawbotctl screen relative-doc.json")));

const relative = extractScreenDocumentPathFromExecParams({
  payload: {
    input: "weclawbotctl screen relative-doc.json --force",
  },
  cwd: "/home/csc/.openclaw/workspace",
});

assert.equal(relative, path.resolve("/home/csc/.openclaw/workspace", "relative-doc.json"));

assert.deepEqual(normalizeDeliveryContext({
  channel: "telegram",
  target: { to: "telegram:5728815108" },
  accountId: "default",
}), {
  channel: "telegram",
  to: "telegram:5728815108",
  accountId: "default",
  threadId: undefined,
});

console.log("openclaw preview hook helpers: ok");
