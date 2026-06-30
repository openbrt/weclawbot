import os from "node:os";
import path from "node:path";

export function extractScreenDocumentPathFromExecParams(params) {
  const command = collectCommandStrings(params).join("\n");
  if (!/weclawbotctl\s+screen\b/u.test(command)) return "";
  for (const line of command.split(/\r?\n/u)) {
    const match = line.match(/(?:^|\s)(?:[^\s;&|]*\/)?weclawbotctl\s+screen\b([^;&|\n]*)/u);
    if (!match) continue;
    const tokens = shellSplit(match[1] || "");
    const candidates = [];
    for (let index = 0; index < tokens.length; index += 1) {
      const token = tokens[index];
      if (!token) continue;
      if (token.startsWith("--")) {
        const key = token.split("=", 1)[0];
        if (!token.includes("=") && new Set(["--credentials", "--timeout"]).has(key)) index += 1;
        continue;
      }
      candidates.push(token);
    }
    const picked = candidates.findLast((token) => token.endsWith(".json")) || candidates.at(-1) || "";
    if (picked) return expandPathWithBase(picked, params?.cwd || params?.workdir || process.cwd());
  }
  return "";
}

export function collectCommandStrings(value, depth = 0) {
  if (depth > 5 || value == null) return [];
  if (typeof value === "string") return value.length <= 40_000 ? [value] : [];
  if (Array.isArray(value)) {
    if (value.every((item) => typeof item === "string")) return [value.join(" ")];
    return value.flatMap((item) => collectCommandStrings(item, depth + 1));
  }
  if (typeof value !== "object") return [];
  return Object.values(value).flatMap((item) => collectCommandStrings(item, depth + 1));
}

export function normalizeDeliveryContext(value) {
  if (!value || typeof value !== "object") return null;
  const channel = typeof value.channel === "string" ? value.channel : "";
  const to = typeof value.to === "string"
    ? value.to
    : typeof value.target?.to === "string"
      ? value.target.to
      : "";
  if (!channel || !to) return null;
  return {
    channel,
    to,
    accountId: typeof value.accountId === "string" ? value.accountId : undefined,
    threadId: value.threadId,
  };
}

function shellSplit(value) {
  const tokens = [];
  let token = "";
  let quote = "";
  let escaped = false;
  for (const ch of String(value || "")) {
    if (escaped) {
      token += ch;
      escaped = false;
      continue;
    }
    if (ch === "\\") {
      escaped = true;
      continue;
    }
    if (quote) {
      if (ch === quote) quote = "";
      else token += ch;
      continue;
    }
    if (ch === "'" || ch === "\"") {
      quote = ch;
      continue;
    }
    if (/\s/u.test(ch)) {
      if (token) {
        tokens.push(token);
        token = "";
      }
      continue;
    }
    token += ch;
  }
  if (token) tokens.push(token);
  return tokens;
}

function expandPathWithBase(value, base) {
  const expanded = expandPath(value);
  return path.isAbsolute(expanded) ? expanded : path.resolve(String(base || process.cwd()), expanded);
}

function expandPath(value) {
  const raw = String(value || "");
  return raw.startsWith("~/") ? path.join(os.homedir(), raw.slice(2)) : raw;
}
