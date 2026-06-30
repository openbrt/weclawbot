#!/usr/bin/env node
import { readFile } from "node:fs/promises";
import { curateEvent } from "../index.js";

const file = process.argv[2] || "eval/sticky-core.jsonl";
const raw = await readFile(file, "utf8");
const cases = raw.split("\n").map((line) => line.trim()).filter(Boolean).map((line) => JSON.parse(line));

let passed = 0;
const failures = [];

for (const [index, testCase] of cases.entries()) {
  const event = testCase.event || {
    version: 1,
    kind: testCase.kind || "wechat_text",
    text: testCase.text,
    filename: testCase.filename,
    blocks: testCase.blocks,
    source: testCase.source,
  };
  const decision = await curateEvent(event, { includeTrace: true });
  const ok = matches(decision, testCase.expect || {});
  if (ok) {
    passed += 1;
  } else {
    failures.push({ index: index + 1, name: testCase.name || "", expect: testCase.expect, got: decision });
  }
}

for (const failure of failures) {
  process.stderr.write(`FAIL ${failure.index} ${failure.name}\n`);
  process.stderr.write(`  expect ${JSON.stringify(failure.expect)}\n`);
  process.stderr.write(`  got    ${JSON.stringify(failure.got)}\n`);
}

process.stdout.write(`sticky-core eval: ${passed}/${cases.length} passed\n`);
if (failures.length > 0) {
  process.exit(1);
}

function matches(decision, expect) {
  if (expect.action && decision.action !== expect.action) {
    return false;
  }
  if (expect.title && decision.note?.title !== expect.title) {
    return false;
  }
  if (expect.contains && !decision.note?.body?.includes(expect.contains)) {
    return false;
  }
  if (expect.no_note && decision.note) {
    return false;
  }
  if (expect.footer && decision.note?.footer !== expect.footer) {
    return false;
  }
  if (expect.reply_contains && !decision.user_reply?.includes(expect.reply_contains)) {
    return false;
  }
  return true;
}
