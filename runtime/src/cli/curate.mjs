#!/usr/bin/env node
import { readFile } from "node:fs/promises";
import { curateEvent } from "../index.js";

const args = process.argv.slice(2);
const useLegacyModel = args.includes("--model");
const useStudent = args.includes("--student");
const useTeacher = args.includes("--teacher") || useLegacyModel;
const input = await readInput(args.filter((arg) => !["--model", "--student", "--teacher"].includes(arg)));
const event = parseEvent(input);
const decision = await curateEvent(event, {
  includeTrace: true,
  models: {
    student: {
      enabled: useStudent,
      provider: "modelscope",
    },
    teacher: {
      enabled: useTeacher,
      provider: "deepseek",
    },
  },
});
process.stdout.write(`${JSON.stringify(decision, null, 2)}\n`);

async function readInput(args) {
  if (args.length > 0) {
    if (args[0] === "--file") {
      return readFile(args[1], "utf8");
    }
    return args.join(" ");
  }

  if (!process.stdin.isTTY) {
    return readStream(process.stdin);
  }

  throw new Error("usage: npm run curate -- <text> OR node src/cli/curate.mjs --file event.json");
}

function parseEvent(raw) {
  const text = raw.trim();
  if (!text) {
    throw new Error("empty input");
  }
  if (text.startsWith("{")) {
    return JSON.parse(text);
  }
  return {
    version: 1,
    kind: "wechat_text",
    text,
  };
}

function readStream(stream) {
  return new Promise((resolve, reject) => {
    let data = "";
    stream.setEncoding("utf8");
    stream.on("data", (chunk) => {
      data += chunk;
    });
    stream.on("end", () => resolve(data));
    stream.on("error", reject);
  });
}
