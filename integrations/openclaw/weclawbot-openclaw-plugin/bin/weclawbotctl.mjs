#!/usr/bin/env node

import crypto from "node:crypto";
import { spawn } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import process from "node:process";

import { validateActivity } from "../lib/activity.mjs";
import { validateScreenDocument } from "../lib/direct-control.mjs";
import { classifyMqttControlError, normalizeCredentials, publishControl, publishControlAndWaitStatus, testConnection } from "../lib/mqtt-control.mjs";
import { createPreviewArtifactDir, writePreviewManifest } from "../lib/preview-manifest.mjs";
import { writeScreenDocumentPreviewFiles } from "../lib/screen-preview.mjs";

const DEFAULT_ENDPOINT = "https://weclawbot.link/byoa";
const DEFAULT_CREDENTIALS_PATH = path.join(os.homedir(), ".config", "weclawbot", "agent-mqtt.json");
const DEFAULT_OPENCLAW_PLUGIN_SPEC = "@openbrt/weclawbotctl";
const MIN_OPENCLAW_VERSION = "2026.6.9";

const [command, ...args] = process.argv.slice(2);
const commands = new Set(["bind", "status", "doctor", "export", "unbind", "thinking", "idle", "screen", "preview", "clear", "openclaw"]);
if (!commands.has(command)) {
  usage();
  process.exit(64);
}

try {
  if (command === "bind") await commandBind(args);
  else if (command === "status") await commandStatus(args);
  else if (command === "doctor") await commandDoctor(args);
  else if (command === "export") await commandExport(args);
  else if (command === "unbind") await commandUnbind(args);
  else if (command === "screen") await commandScreen(args);
  else if (command === "preview") await commandPreview(args);
  else if (command === "clear") await commandClear(args);
  else if (command === "openclaw") await commandOpenClaw(args);
  else await commandActivity(command, args);
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exit(1);
}

async function commandBind(values) {
  const options = parseOptions(values, {
    name: "user-agent",
    endpoint: process.env.WEC_BYOA_ENDPOINT || DEFAULT_ENDPOINT,
    credentials: credentialsPath(),
  });
  const code = String(options._[0] || "").replace(/\D/gu, "");
  if (!/^\d{6}$/u.test(code)) {
    throw new Error("Usage: weclawbotctl bind <six-digit-code> [--name agent-name]");
  }
  const response = await fetch(options.endpoint, {
    method: "POST",
    headers: { "content-type": "application/json", accept: "application/json" },
    body: JSON.stringify({
      schema: "weclawbot.byoa.v1",
      operation: "claim",
      code,
      agent_name: String(options.name || "user-agent").slice(0, 80),
    }),
    signal: AbortSignal.timeout(15_000),
  });
  const payload = await response.json().catch(() => null);
  if (!response.ok || !payload?.ok || payload?.schema !== "weclawbot.byoa.agent_credentials.v1") {
    throw new Error(`WeClawBot pairing failed: ${payload?.error || `HTTP ${response.status}`}`);
  }

  const file = expandPath(options.credentials);
  await fs.mkdir(path.dirname(file), { recursive: true, mode: 0o700 });
  await fs.writeFile(file, `${JSON.stringify({
    schema: "weclawbot.agent_credentials.v1",
    binding: payload.binding,
    mqtt: payload.mqtt,
    delivery: payload.delivery,
    created_at: new Date().toISOString(),
  }, null, 2)}\n`, { mode: 0o600 });
  await fs.chmod(file, 0o600);
  console.log(`WeClawBot paired with ${payload.binding.device_id}. Credentials saved to ${file}.`);
}

async function commandStatus(values) {
  const options = parseOptions(values, { credentials: credentialsPath(), json: false });
  const file = expandPath(options.credentials);
  const payload = await readCredentials(file);
  if (!payload) {
    const status = { ok: false, paired: false, credentials_path: file };
    printStatus(status, options.json);
    process.exitCode = 1;
    return;
  }
  const config = normalizeCredentials(payload);
  const stat = await fs.stat(file).catch(() => null);
  printStatus({
    ok: true,
    paired: true,
    credentials_path: file,
    file_mode: stat ? modeString(stat.mode) : "",
    binding: payload.binding || {},
    mqtt: maskedMqtt(config, payload.mqtt?.topics || {}),
    delivery: payload.delivery || {},
  }, options.json);
}

async function commandDoctor(values) {
  const options = parseOptions(values, {
    credentials: credentialsPath(),
    json: false,
    online: false,
    timeout: 12,
  });
  const file = expandPath(options.credentials);
  const checks = [];
  const payload = await readCredentials(file);
  checks.push({ name: "credentials_file", ok: Boolean(payload), detail: file });
  if (!payload) {
    printDoctor(checks, options.json);
    process.exitCode = 1;
    return;
  }
  const stat = await fs.stat(file).catch(() => null);
  checks.push({
    name: "credentials_permissions",
    ok: Boolean(stat) && (stat.mode & 0o077) === 0,
    detail: stat ? modeString(stat.mode) : "missing",
  });
  let config = null;
  try {
    config = normalizeCredentials(payload);
    checks.push({ name: "mqtt_profile", ok: true, detail: `${config.url} ${config.clientId}` });
    checks.push({ name: "mqtt_url_tls", ok: config.url.startsWith("wss://"), detail: config.url });
    checks.push({
      name: "client_id_contains_no_secret",
      ok: !/(token|secret|password|passwd|key)/iu.test(config.clientId),
      detail: config.clientId,
    });
  } catch (error) {
    checks.push({ name: "mqtt_profile", ok: false, detail: error.message });
  }
  if (options.online && config) {
    try {
      await testConnection(payload, { timeoutMs: Math.max(1, Number(options.timeout) || 12) * 1000 });
      checks.push({ name: "mqtt_online", ok: true, detail: "connected" });
    } catch (error) {
      const mqttError = classifyMqttControlError(error);
      checks.push({
        name: "mqtt_online",
        ok: false,
        detail: mqttError.message,
        hint: mqttError.code === "credential_revoked_or_not_current_owner"
          ? "This local profile is no longer the current owner. Re-pair with the six-digit code shown on the device."
          : "Check screen power, Wi-Fi, and broker reachability, then retry.",
        error_detail: mqttError.detail,
      });
    }
  }
  printDoctor(checks, options.json);
  if (checks.some((check) => !check.ok)) process.exitCode = 1;
}

async function commandExport(values) {
  const options = parseOptions(values, {
    credentials: credentialsPath(),
    format: "env",
    output: "",
    "include-secret": false,
  });
  const payload = await requireCredentials(expandPath(options.credentials));
  const config = normalizeCredentials(payload);
  const includeSecret = Boolean(options["include-secret"]);
  let body = "";
  if (options.format === "json") {
    body = `${JSON.stringify(maskedExport(payload, includeSecret), null, 2)}\n`;
  } else if (options.format === "env") {
    body = [
      `WEC_MQTT_URL=${shellValue(config.url)}`,
      `WEC_MQTT_CLIENT_ID=${shellValue(config.clientId)}`,
      `WEC_MQTT_USERNAME=${shellValue(config.username)}`,
      `WEC_MQTT_PASSWORD=${shellValue(includeSecret ? config.password : "********")}`,
      `WEC_MQTT_CONTROL_TOPIC=${shellValue(config.controlTopic)}`,
      "",
    ].join("\n");
  } else if (options.format === "mosquitto") {
    body = [
      `url ${config.url}`,
      `id ${config.clientId}`,
      `username ${config.username}`,
      `password ${includeSecret ? config.password : "********"}`,
      "protocol-version mqttv5",
      "clean-session true",
      "",
    ].join("\n");
  } else {
    throw new Error("export format must be env, json, or mosquitto");
  }
  if (options.output) {
    const file = expandPath(options.output);
    await fs.mkdir(path.dirname(file), { recursive: true, mode: 0o700 });
    await fs.writeFile(file, body, { mode: includeSecret ? 0o600 : 0o644 });
    if (includeSecret) await fs.chmod(file, 0o600);
    console.log(`Wrote ${options.format} profile to ${file}.`);
  } else {
    process.stdout.write(body);
  }
}

async function commandUnbind(values) {
  const options = parseOptions(values, { credentials: credentialsPath(), yes: false });
  if (!options.yes) {
    throw new Error("This only removes the local credential file. Re-run with --yes.");
  }
  const file = expandPath(options.credentials);
  await fs.rm(file, { force: true });
  console.log(`Removed local WeClawBot credentials: ${file}`);
}

async function commandScreen(values) {
  const options = parseOptions(values, {
    credentials: credentialsPath(),
    force: false,
    wait: true,
    timeout: 12,
    preview: true,
    "preview-dir": "",
    "preview-manifest": true,
    scale: 2,
  });
  const file = String(options._[0] || "").trim();
  if (!file || options._.length !== 1) {
    throw new Error("Usage: weclawbotctl screen <document.json> [--force] [--no-wait] [--timeout seconds] [--preview-dir dir]");
  }
  const document = JSON.parse(await fs.readFile(file, "utf8"));
  if (options.force) {
    document.force_replace = true;
    document.base_revision = "*";
  }
  const validation = validateScreenDocument(document, {
    agent_transport: { available: true, screen_document_available: true },
  });
  if (!validation.ok) {
    throw new Error(`Invalid screen document: ${validation.errors.join("; ")}`);
  }
  const previewOutputDir = options["preview-dir"] || (options["preview-manifest"] ? await createPreviewArtifactDir(document.id || "screen") : "");
  const preview = options.preview
    ? await writeScreenDocumentPreviewFiles(document, {
      outputDir: previewOutputDir,
      scale: Number(options.scale) || 2,
    })
    : { available: false, pages: [] };
  const control = {
    schema: "weclawbot.control.v1",
    id: `screen_${crypto.randomUUID()}`,
    kind: "screen_document",
    document,
  };
  const credentials = await requireCredentials(expandPath(options.credentials));
  if (options.wait) {
    const delivery = await publishControlAndWaitStatus(credentials, control, {
      expectedDetail: document.id,
      timeoutMs: Math.max(1, Number(options.timeout) || 12) * 1000,
    });
    if (delivery.status.kind !== "applied") {
      throw new Error(`Device rejected screen document: ${delivery.status.detail || "unknown"}`);
    }
    const manifest = options["preview-manifest"]
      ? await writePreviewManifest({
        document,
        preview,
        source: "weclawbotctl_screen",
        status: { applied: true, mqtt_status: delivery.status },
      })
      : null;
    console.log(JSON.stringify({
      ok: true,
      published: true,
      applied: true,
      id: document.id,
      pages: document.pages.length,
      force_replace: document.force_replace === true,
      warnings: validation.warnings,
      layout_guidance: validation.layout_guidance,
      preview,
      preview_manifest: previewManifestSummary(manifest),
      status: delivery.status,
    }));
    return;
  }
  await publishControl(credentials, control);
  const manifest = options["preview-manifest"]
    ? await writePreviewManifest({
      document,
      preview,
      source: "weclawbotctl_screen",
      status: { applied: null },
    })
    : null;
  console.log(JSON.stringify({
    ok: true,
    published: true,
    applied: null,
    id: document.id,
    pages: document.pages.length,
    force_replace: document.force_replace === true,
    warnings: validation.warnings,
    layout_guidance: validation.layout_guidance,
    preview,
    preview_manifest: previewManifestSummary(manifest),
  }));
}

async function commandPreview(values) {
  const options = parseOptions(values, {
    output: "",
    "output-dir": "",
    scale: 2,
  });
  const file = String(options._[0] || "").trim();
  if (!file || options._.length !== 1) {
    throw new Error("Usage: weclawbotctl preview <document.json> [--output-dir dir] [--scale 1..4]");
  }
  const document = JSON.parse(await fs.readFile(file, "utf8"));
  const validation = validateScreenDocument(document, {
    agent_transport: { available: true, screen_document_available: true },
  });
  if (!validation.ok) {
    throw new Error(`Invalid screen document: ${validation.errors.join("; ")}`);
  }
  const outputDir = options["output-dir"] || options.output || "";
  const preview = await writeScreenDocumentPreviewFiles(document, {
    outputDir,
    scale: Number(options.scale) || 2,
  });
  console.log(JSON.stringify({
    ok: true,
    id: document.id,
    pages: document.pages.length,
    warnings: validation.warnings,
    layout_guidance: validation.layout_guidance,
    preview,
  }));
}

async function commandClear(values) {
  const options = parseOptions(values, {
    credentials: credentialsPath(),
    target: "note",
    wait: true,
    timeout: 12,
  });
  if (options._.length > 1) {
    throw new Error("Usage: weclawbotctl clear [--target note|idle_photo] [--no-wait] [--timeout seconds]");
  }
  const target = normalizeClearTarget(options._[0] || options.target);
  const control = {
    schema: "weclawbot.control.v1",
    id: `clear_${crypto.randomUUID()}`,
    kind: "screen_clear",
    target,
  };
  const credentials = await requireCredentials(expandPath(options.credentials));
  if (options.wait) {
    const delivery = await publishControlAndWaitStatus(credentials, control, {
      expectedDetail: clearStatusDetail(target),
      timeoutMs: Math.max(1, Number(options.timeout) || 12) * 1000,
    });
    if (delivery.status.kind !== "applied") {
      throw new Error(`Device rejected screen clear: ${delivery.status.detail || "unknown"}`);
    }
    console.log(JSON.stringify({
      ok: true,
      published: true,
      applied: true,
      target,
      status: delivery.status,
    }));
    return;
  }
  await publishControl(credentials, control);
  console.log(JSON.stringify({ ok: true, published: true, applied: null, target }));
}

async function commandActivity(state, values) {
  const options = parseOptions(values, {
    credentials: credentialsPath(),
    ttl: 45,
    id: "",
  });
  const correlationId = options.id || crypto.randomUUID();
  const activity = {
    schema: "weclawbot.activity.v1",
    state,
    correlation_id: correlationId,
    ...(state === "thinking" ? { ttl_seconds: Number(options.ttl) } : {}),
  };
  const validation = validateActivity(activity, { agent_transport: { available: true, activity_available: true } });
  if (!validation.ok) {
    throw new Error(`Invalid activity: ${validation.errors.join("; ")}`);
  }

  await publishControl(await requireCredentials(expandPath(options.credentials)), {
    schema: "weclawbot.control.v1",
    id: `activity_${crypto.randomUUID()}`,
    kind: "activity",
    activity,
  });
  console.log(JSON.stringify({ ok: true, state: activity.state, correlation_id: correlationId }));
}

async function commandOpenClaw(values) {
  const first = String(values[0] || "");
  const subcommand = first && !first.startsWith("--") ? first : "doctor";
  const rest = first && !first.startsWith("--") ? values.slice(1) : values;
  if (subcommand === "install") {
    await commandOpenClawInstall(rest);
  } else if (subcommand === "doctor") {
    await commandOpenClawDoctor(rest);
  } else {
    throw new Error("Usage: weclawbotctl openclaw install|doctor");
  }
}

async function commandOpenClawInstall(values) {
  const options = parseOptions(values, {
    bin: process.env.OPENCLAW_BIN || "",
    spec: DEFAULT_OPENCLAW_PLUGIN_SPEC,
    force: true,
    doctor: true,
  });
  const openclaw = await resolveOpenClawBin(options.bin);
  const spec = String(options.spec || DEFAULT_OPENCLAW_PLUGIN_SPEC);
  const version = await runCaptured(openclaw, ["--version"], { timeoutMs: 10_000 });
  const versionCheck = openClawVersionCheck(version.stdout);
  if (version.code !== 0) throw new Error(`OpenClaw CLI not found. Install OpenClaw or set OPENCLAW_BIN=/path/to/openclaw.`);
  if (!versionCheck.ok) throw new Error(`${versionCheck.detail}. ${versionCheck.hint}`);
  const installArgs = ["plugins", "install", spec, "--pin"];
  if (options.force) installArgs.push("--force");
  await runRequired(openclaw, installArgs);
  await runRequired(openclaw, ["plugins", "enable", "weclawbot"]);
  await runRequired(openclaw, [
    "config",
    "set",
    "plugins.entries.weclawbot.hooks.allowConversationAccess",
    "true",
    "--strict-json",
  ]);
  console.log("OpenClaw plugin installed and enabled. Restart the OpenClaw gateway or app so it reloads plugins.");
  if (options.doctor) {
    await commandOpenClawDoctor(["--bin", openclaw, "--gateway=false"]);
  }
}

async function commandOpenClawDoctor(values) {
  const options = parseOptions(values, {
    bin: process.env.OPENCLAW_BIN || "",
    json: false,
    gateway: true,
    timeout: 20,
  });
  const openclaw = await resolveOpenClawBin(options.bin);
  const timeoutMs = Math.max(1, Number(options.timeout) || 20) * 1000;
  const checks = [];

  const version = await runCaptured(openclaw, ["--version"], { timeoutMs });
  pushProcessCheck(checks, "openclaw_bin", version, "OpenClaw CLI found");
  if (version.code === 0) checks.push(openClawVersionCheck(version.stdout));

  const inspect = await runCaptured(openclaw, ["plugins", "inspect", "weclawbot"], { timeoutMs });
  checks.push({
    name: "openclaw_plugin",
    ok: inspect.code === 0,
    detail: inspect.code === 0 ? summarizeOpenClawPlugin(inspect.stdout) : compactText(inspect.stderr || inspect.stdout || "not installed"),
    hint: inspect.code === 0 ? "" : `Run: weclawbotctl openclaw install`,
  });

  const pluginDoctor = await runCaptured(openclaw, ["plugins", "doctor"], { timeoutMs });
  const doctorText = `${pluginDoctor.stdout}\n${pluginDoctor.stderr}`;
  const weclawbotIssue = /(^|\n)\s*-\s*weclawbot:|weclawbot.*contracts\.tools|weclawbot.*plugin/i.test(doctorText);
  checks.push({
    name: "openclaw_plugin_doctor",
    ok: pluginDoctor.code === 0 && !weclawbotIssue,
    detail: pluginDoctor.code === 0 && !weclawbotIssue
      ? "no WeClawBot plugin issues detected"
      : compactText(doctorText || `exit ${pluginDoctor.code}`),
    hint: weclawbotIssue ? "Upgrade and reinstall: weclawbotctl openclaw install" : "",
  });

  const hooksAccess = await runCaptured(openclaw, [
    "config",
    "get",
    "plugins.entries.weclawbot.hooks.allowConversationAccess",
    "--json",
  ], { timeoutMs });
  const hooksEnabled = hooksAccess.code === 0 && /^\s*true\s*$/iu.test(hooksAccess.stdout);
  checks.push({
    name: "openclaw_weclawbot_hooks",
    ok: hooksEnabled,
    detail: hooksEnabled
      ? "conversation hooks enabled for automatic thinking state"
      : compactText(hooksAccess.stderr || hooksAccess.stdout || "hooks.allowConversationAccess is not enabled"),
    hint: hooksEnabled ? "" : "Run: openclaw config set plugins.entries.weclawbot.hooks.allowConversationAccess true --strict-json",
  });

  if (options.gateway) {
    const gatewayEnv = { ...process.env };
    const defaultCert = path.join(os.homedir(), ".openclaw", "gateway", "tls", "gateway-cert.pem");
    if (!gatewayEnv.NODE_EXTRA_CA_CERTS && await fileExists(defaultCert)) {
      gatewayEnv.NODE_EXTRA_CA_CERTS = defaultCert;
    }
    const status = await runCaptured(openclaw, ["status", "--json"], { env: gatewayEnv, timeoutMs });
    checks.push(gatewayStatusCheck(status, gatewayEnv.NODE_EXTRA_CA_CERTS || ""));
  }

  printOpenClawDoctor(checks, options.json);
  if (checks.some((check) => !check.ok)) process.exitCode = 1;
}

function parseOptions(values, defaults = {}) {
  const options = { ...defaults, _: [] };
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (!value.startsWith("--")) {
      options._.push(value);
      continue;
    }
    const [rawKey, inlineValue] = value.slice(2).split("=", 2);
    const key = rawKey.trim();
    if (!key) continue;
    if (key.startsWith("no-")) {
      const positive = key.slice(3);
      if (typeof options[positive] === "boolean" && inlineValue === undefined) {
        options[positive] = false;
        continue;
      }
    }
    if (typeof options[key] === "boolean") {
      options[key] = inlineValue === undefined ? true : !/^(0|false|no|off)$/iu.test(inlineValue);
    } else {
      options[key] = inlineValue === undefined ? String(values[++index] || "") : inlineValue;
    }
  }
  return options;
}

function credentialsPath() {
  return process.env.WEC_AGENT_CREDENTIALS_PATH || DEFAULT_CREDENTIALS_PATH;
}

async function readCredentials(file) {
  try {
    const payload = JSON.parse(await fs.readFile(file, "utf8"));
    return payload && typeof payload === "object" ? payload : null;
  } catch {
    return null;
  }
}

async function requireCredentials(file) {
  const payload = await readCredentials(file);
  if (!payload) throw new Error(`WeClawBot is not paired. Run: weclawbotctl bind <six-digit-code>`);
  return payload;
}

function expandPath(value) {
  const raw = String(value || DEFAULT_CREDENTIALS_PATH);
  return raw.startsWith("~/") ? path.join(os.homedir(), raw.slice(2)) : raw;
}

function maskedMqtt(config, topics) {
  return {
    url: config.url,
    client_id: config.clientId,
    username: config.username,
    password: "********",
    topics: {
      control: config.controlTopic,
      events: topics.events || "",
      status: topics.status || "",
    },
  };
}

function maskedExport(payload, includeSecret) {
  const copy = JSON.parse(JSON.stringify(payload));
  if (!includeSecret && copy?.mqtt?.password) copy.mqtt.password = "********";
  return copy;
}

function printStatus(status, json) {
  if (json) {
    console.log(JSON.stringify(status, null, 2));
    return;
  }
  if (!status.paired) {
    console.log(`WeClawBot not paired. Credentials: ${status.credentials_path}`);
    return;
  }
  console.log(`WeClawBot paired: ${status.binding?.device_id || "device"}`);
  console.log(`Credentials: ${status.credentials_path} (${status.file_mode || "unknown mode"})`);
  console.log(`MQTT: ${status.mqtt.url}`);
  console.log(`Client ID: ${status.mqtt.client_id}`);
  console.log(`Control topic: ${status.mqtt.topics.control}`);
}

function printDoctor(checks, json) {
  if (json) {
    console.log(JSON.stringify({ ok: checks.every((check) => check.ok), checks }, null, 2));
    return;
  }
  for (const check of checks) {
    console.log(`${check.ok ? "ok" : "fail"} ${check.name}: ${check.detail}`);
    if (!check.ok && check.hint) console.log(`hint ${check.name}: ${check.hint}`);
  }
}

function printOpenClawDoctor(checks, json) {
  if (json) {
    console.log(JSON.stringify({ ok: checks.every((check) => check.ok), checks }, null, 2));
    return;
  }
  for (const check of checks) {
    console.log(`${check.ok ? "ok" : "fail"} ${check.name}: ${check.detail}`);
    if (!check.ok && check.hint) console.log(`hint ${check.name}: ${check.hint}`);
  }
}

async function runRequired(file, args) {
  const result = await runProcess(file, args, { stdio: "inherit" });
  if (result.code !== 0) {
    throw new Error(`${file} ${args.join(" ")} failed with exit ${result.code}`);
  }
}

async function runCaptured(file, args, options = {}) {
  return runProcess(file, args, { ...options, stdio: "pipe" });
}

function runProcess(file, args, options = {}) {
  return new Promise((resolve) => {
    const child = spawn(file, args, {
      env: options.env || process.env,
      stdio: options.stdio === "inherit" ? "inherit" : ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    const timeoutMs = Number(options.timeoutMs || 0);
    const timeout = timeoutMs > 0 ? setTimeout(() => {
      timedOut = true;
      child.kill("SIGTERM");
    }, timeoutMs) : null;
    if (child.stdout) child.stdout.on("data", (chunk) => { stdout += chunk; });
    if (child.stderr) child.stderr.on("data", (chunk) => { stderr += chunk; });
    child.on("error", (error) => {
      if (timeout) clearTimeout(timeout);
      resolve({ code: 127, stdout, stderr: error.message, timedOut });
    });
    child.on("close", (code) => {
      if (timeout) clearTimeout(timeout);
      resolve({ code: timedOut ? 124 : (code ?? 1), stdout, stderr, timedOut });
    });
  });
}

function pushProcessCheck(checks, name, result, okDetail) {
  checks.push({
    name,
    ok: result.code === 0,
    detail: result.code === 0 ? compactText(result.stdout || okDetail) : compactText(result.stderr || result.stdout || `exit ${result.code}`),
    hint: result.code === 127 ? "Install OpenClaw or set OPENCLAW_BIN=/path/to/openclaw." : "",
  });
}

function openClawVersionCheck(output) {
  const version = parseOpenClawVersion(output);
  if (!version) {
    return {
      name: "openclaw_version",
      ok: true,
      detail: compactText(output || "version unknown"),
      hint: "",
    };
  }
  const ok = compareDottedVersion(version, MIN_OPENCLAW_VERSION) >= 0;
  return {
    name: "openclaw_version",
    ok,
    detail: `OpenClaw ${version} (requires >= ${MIN_OPENCLAW_VERSION})`,
    hint: ok ? "" : "Upgrade OpenClaw before installing the WeClawBot plugin tools.",
  };
}

function parseOpenClawVersion(output) {
  const match = String(output || "").match(/OpenClaw\s+([0-9]+(?:\.[0-9]+){1,3})/u);
  return match ? match[1] : "";
}

function compareDottedVersion(left, right) {
  const a = String(left).split(".").map((part) => Number(part) || 0);
  const b = String(right).split(".").map((part) => Number(part) || 0);
  const length = Math.max(a.length, b.length);
  for (let index = 0; index < length; index += 1) {
    const diff = (a[index] || 0) - (b[index] || 0);
    if (diff !== 0) return diff > 0 ? 1 : -1;
  }
  return 0;
}

function summarizeOpenClawPlugin(output) {
  const lines = String(output || "").split(/\r?\n/u);
  const wanted = [];
  for (const line of lines) {
    if (/^(Status|Version|Source|Install path|Spec):/u.test(line.trim())) wanted.push(line.trim());
  }
  return wanted.length ? wanted.join("; ") : "installed";
}

function gatewayStatusCheck(result, extraCaCerts) {
  const text = `${result.stdout}\n${result.stderr}`;
  if (result.code !== 0) {
    return {
      name: "openclaw_gateway_status",
      ok: false,
      detail: compactText(text || `exit ${result.code}`),
      hint: tlsHint(text, extraCaCerts),
    };
  }
  try {
    const status = JSON.parse(result.stdout);
    const gateway = status.gateway || {};
    const ok = gateway.reachable === true;
    return {
      name: "openclaw_gateway_status",
      ok,
      detail: ok ? `reachable ${gateway.url || ""}`.trim() : compactText(gateway.error || "gateway not reachable"),
      hint: ok ? "" : tlsHint(gateway.error || text, extraCaCerts),
    };
  } catch {
    return {
      name: "openclaw_gateway_status",
      ok: false,
      detail: compactText(text || "status JSON parse failed"),
      hint: tlsHint(text, extraCaCerts),
    };
  }
}

function tlsHint(text, extraCaCerts) {
  const value = String(text || "");
  if (/self-signed certificate/i.test(value)) {
    return extraCaCerts
      ? `Retry with NODE_EXTRA_CA_CERTS=${extraCaCerts}, or use a gateway certificate trusted by Node.`
      : "If your OpenClaw gateway uses a self-signed cert, set NODE_EXTRA_CA_CERTS to its CA/cert path.";
  }
  if (/ALTNAME|Hostname\/IP does not match certificate/i.test(value)) {
    return "Regenerate the OpenClaw gateway certificate with SANs for localhost and 127.0.0.1, or configure OpenClaw to use a valid certificate.";
  }
  return "";
}

async function fileExists(file) {
  try {
    await fs.access(file);
    return true;
  } catch {
    return false;
  }
}

async function resolveOpenClawBin(value) {
  if (value) return String(value);
  const scriptDir = path.dirname(process.argv[1] || "");
  const candidates = [
    scriptDir ? path.join(scriptDir, "openclaw") : "",
    path.join(os.homedir(), ".npm-global", "bin", "openclaw"),
    path.join(os.homedir(), ".local", "bin", "openclaw"),
    "/usr/local/bin/openclaw",
    "/opt/homebrew/bin/openclaw",
  ].filter(Boolean);
  for (const candidate of candidates) {
    if (await fileExists(candidate)) return candidate;
  }
  return "openclaw";
}

function compactText(value) {
  return String(value || "").replace(/\s+/gu, " ").trim().slice(0, 500);
}

function normalizeClearTarget(value) {
  const target = String(value || "note").trim();
  if (target === "note" || target === "idle_photo" || target === "photo") {
    return target === "photo" ? "idle_photo" : target;
  }
  throw new Error("clear target must be note or idle_photo");
}

function clearStatusDetail(target) {
  return target === "idle_photo" ? "clear_idle_photo" : "clear_note";
}

function previewManifestSummary(manifest) {
  if (!manifest) return null;
  return {
    id: manifest.id,
    path: manifest.path,
    pages: manifest.preview?.pages?.length || 0,
  };
}

function shellValue(value) {
  return `'${String(value).replaceAll("'", "'\\''")}'`;
}

function modeString(mode) {
  return `0${(mode & 0o777).toString(8)}`;
}

function usage() {
  console.error(`Usage:
  weclawbotctl bind <six-digit-code> [--name agent-name]
  weclawbotctl status [--json]
  weclawbotctl doctor [--online] [--json] [--timeout seconds]
  weclawbotctl export [--format env|json|mosquitto] [--include-secret] [--output file]
  weclawbotctl unbind --yes
  weclawbotctl thinking [--ttl seconds] [--id correlation-id]
  weclawbotctl idle [--id correlation-id]
  weclawbotctl preview <document.json> [--output-dir dir] [--scale 1..4]
  weclawbotctl screen <document.json> [--force] [--no-wait] [--timeout seconds] [--preview-dir dir] [--no-preview] [--no-preview-manifest]
  weclawbotctl clear [--target note|idle_photo] [--no-wait] [--timeout seconds]
  weclawbotctl openclaw install [--spec @openbrt/weclawbotctl] [--bin /path/to/openclaw] [--force=false]
  weclawbotctl openclaw doctor [--gateway=false] [--bin /path/to/openclaw] [--json]`);
}
