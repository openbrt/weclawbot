import crypto from "node:crypto";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { Type } from "typebox";
import { defineToolPlugin } from "openclaw/plugin-sdk/tool-plugin";

import { validateActivity } from "./lib/activity.mjs";
import { validateScreenDocument } from "./lib/direct-control.mjs";
import { normalizeCredentials, publishControl, publishControlAndWaitStatus, testConnection } from "./lib/mqtt-control.mjs";
import { collectCommandStrings, extractScreenDocumentPathFromExecParams, normalizeDeliveryContext } from "./lib/openclaw-preview.mjs";
import { createPreviewArtifactDir, listRecentPreviewManifests, markPreviewManifest, writePreviewManifest } from "./lib/preview-manifest.mjs";
import { renderScreenDocumentPreviewPages, writeScreenDocumentPreviewFiles } from "./lib/screen-preview.mjs";

const DEFAULT_CREDENTIALS_PATH = path.join(os.homedir(), ".config", "weclawbot", "agent-mqtt.json");
const SCREEN_PROMPT_PATTERN = /weclawbot|weclaw|微笺|桌宠|墨水屏|电子墨水|屏上|上屏|发到屏|推到屏|放到屏|显示到屏|发送到屏|清屏|清理屏幕|屏幕/iu;
const activeRunActivities = new Map();
const activePreviewWindows = new Map();

// The long-running curator bridge remains a separate service. These tools keep
// the local agent path explicit: validate first, then publish only with the
// user's paired MQTT credential.
const pluginEntry = defineToolPlugin({
  id: "weclawbot",
  name: "WeClawBot",
  description: "WeClawBot screen-curation skill pack and direct MQTT control tools.",
  configSchema: Type.Object({
    auto_activity: Type.Optional(Type.Boolean()),
    auto_preview: Type.Optional(Type.Boolean()),
  }, { additionalProperties: false }),
  tools: (tool) => [
    tool({
      name: "weclawbot_status",
      label: "Check WeClawBot pairing",
      description: "Read the local WeClawBot MQTT pairing profile and optionally test the live MQTT connection.",
      parameters: Type.Object({
        credentials_path: Type.Optional(Type.String()),
        online: Type.Optional(Type.Boolean()),
      }, { additionalProperties: false }),
      execute: async ({ credentials_path, online }) => {
        const file = expandPath(credentials_path || DEFAULT_CREDENTIALS_PATH);
        const payload = await readCredentials(file);
        if (!payload) {
          return { ok: false, paired: false, credentials_path: file, error: "not_paired" };
        }
        const config = normalizeCredentials(payload);
        const result = {
          ok: true,
          paired: true,
          credentials_path: file,
          binding: payload.binding || {},
          mqtt: maskedMqtt(config, payload.mqtt?.topics || {}),
        };
        if (online) {
          await testConnection(payload);
          result.online = true;
        }
        return result;
      },
    }),
    tool({
      name: "weclawbot_validate_screen_document",
      label: "Validate WeClawBot screen document",
      description: "Validate a candidate direct screen document against the firmware-supplied device_context. This does not send or queue anything.",
      parameters: Type.Object({
        document: Type.Any(),
        device_context: Type.Optional(Type.Any()),
      }, { additionalProperties: false }),
      execute: ({ document, device_context }) => validateScreenDocument(document, device_context),
    }),
    tool({
      name: "weclawbot_clear_screen",
      label: "Clear WeClawBot screen",
      description: "Clear the paired WeClawBot note or idle-photo state with the firmware screen_clear control. Do not emulate clearing by publishing a blank or black bitmap.",
      parameters: Type.Object({
        target: Type.Optional(Type.String()),
        credentials_path: Type.Optional(Type.String()),
        wait_status: Type.Optional(Type.Boolean()),
        timeout_seconds: Type.Optional(Type.Number()),
      }, { additionalProperties: false }),
      execute: async ({ target, credentials_path, wait_status, timeout_seconds }) => {
        const clearTarget = normalizeClearTarget(target);
        const control = {
          schema: "weclawbot.control.v1",
          id: `clear_${crypto.randomUUID()}`,
          kind: "screen_clear",
          target: clearTarget,
        };
        const credentials = await requireCredentials(credentials_path);
        if (wait_status !== false) {
          const delivery = await publishControlAndWaitStatus(credentials, control, {
            expectedDetail: clearStatusDetail(clearTarget),
            timeoutMs: Math.max(1, Number(timeout_seconds) || 12) * 1000,
          });
          return {
            ok: delivery.status.kind === "applied",
            published: true,
            applied: delivery.status.kind === "applied",
            rejected: delivery.status.kind === "rejected",
            target: clearTarget,
            status: delivery.status,
          };
        }
        await publishControl(credentials, control);
        return { ok: true, published: true, applied: null, target: clearTarget };
      },
    }),
    tool({
      name: "weclawbot_publish_screen_document",
      label: "Publish WeClawBot screen document",
      description: "Validate and publish a pre-rendered mono1 screen document through the paired local MQTT profile. The document must already contain pixels; this tool does not lay out text.",
      parameters: Type.Object({
        document: Type.Any(),
        device_context: Type.Optional(Type.Any()),
        credentials_path: Type.Optional(Type.String()),
        force_replace: Type.Optional(Type.Boolean()),
        wait_status: Type.Optional(Type.Boolean()),
        timeout_seconds: Type.Optional(Type.Number()),
        preview_output_dir: Type.Optional(Type.String()),
      }, { additionalProperties: false }),
      execute: async ({ document, device_context, credentials_path, force_replace, wait_status, timeout_seconds, preview_output_dir }) => {
        const outbound = cloneObject(document);
        if (force_replace) {
          outbound.force_replace = true;
          outbound.base_revision = "*";
        }
        const validation = validateScreenDocument(outbound, device_context || {
          agent_transport: { available: true, screen_document_available: true },
        });
        if (!validation.ok) {
          return { ok: false, published: false, errors: validation.errors, validation };
        }
        const control = {
          schema: "weclawbot.control.v1",
          id: `screen_${crypto.randomUUID()}`,
          kind: "screen_document",
          document: outbound,
        };
        const credentials = await requireCredentials(credentials_path);
        const previewDir = preview_output_dir || await createPreviewArtifactDir(outbound.id || "screen");
        const preview = await writeScreenDocumentPreviewFiles(outbound, { outputDir: previewDir });
        if (wait_status !== false) {
          const delivery = await publishControlAndWaitStatus(credentials, control, {
            expectedDetail: outbound.id,
            timeoutMs: Math.max(1, Number(timeout_seconds) || 12) * 1000,
          });
          const manifest = delivery.status.kind === "applied"
            ? await writePreviewManifest({
              document: outbound,
              preview,
              source: "openclaw_tool",
              status: { applied: true, mqtt_status: delivery.status },
            })
            : null;
          return {
            ok: delivery.status.kind === "applied",
            published: true,
            applied: delivery.status.kind === "applied",
            rejected: delivery.status.kind === "rejected",
            id: outbound.id,
            pages: outbound.pages.length,
            force_replace: outbound.force_replace === true,
            warnings: validation.warnings,
            layout_guidance: validation.layout_guidance,
            preview,
            preview_manifest: previewManifestSummary(manifest),
            status: delivery.status,
          };
        }
        await publishControl(credentials, control);
        const manifest = await writePreviewManifest({
          document: outbound,
          preview,
          source: "openclaw_tool",
          status: { applied: null },
        });
        return {
          ok: true,
          published: true,
          applied: null,
          id: outbound.id,
          pages: outbound.pages.length,
          force_replace: outbound.force_replace === true,
          warnings: validation.warnings,
          layout_guidance: validation.layout_guidance,
          preview,
          preview_manifest: previewManifestSummary(manifest),
        };
      },
    }),
    tool({
      name: "weclawbot_validate_activity",
      label: "Validate WeClawBot activity",
      description: "Validate a temporary thinking or idle activity message. It does not send or queue anything.",
      parameters: Type.Object({
        activity: Type.Any(),
        device_context: Type.Optional(Type.Any()),
      }, { additionalProperties: false }),
      execute: ({ activity, device_context }) => validateActivity(activity, device_context),
    }),
    tool({
      name: "weclawbot_publish_activity",
      label: "Publish WeClawBot activity",
      description: "Validate and publish a short-lived thinking or idle activity through the paired local MQTT profile.",
      parameters: Type.Object({
        activity: Type.Any(),
        device_context: Type.Optional(Type.Any()),
        credentials_path: Type.Optional(Type.String()),
      }, { additionalProperties: false }),
      execute: async ({ activity, device_context, credentials_path }) => {
        const validation = validateActivity(activity, device_context || {
          agent_transport: { available: true, activity_available: true },
        });
        if (!validation.ok) {
          return { ok: false, published: false, errors: validation.errors, validation };
        }
        await publishControl(await requireCredentials(credentials_path), {
          schema: "weclawbot.control.v1",
          id: `activity_${crypto.randomUUID()}`,
          kind: "activity",
          activity,
        });
        return {
          ok: true,
          published: true,
          state: activity.state,
          correlation_id: activity.correlation_id,
        };
      },
    }),
  ],
});

const registerTools = pluginEntry.register;
pluginEntry.register = (api) => {
  registerTools(api);
  registerOpenClawHooks(api);
};

export default pluginEntry;

function registerOpenClawHooks(api) {
  if (!api || typeof api.on !== "function") return;
  api.on("before_agent_run", async (event, ctx) => {
    await startHookActivity(api, event, ctx);
    return { outcome: "pass" };
  }, { timeoutMs: 5_000 });
  api.on("before_agent_finalize", async (event, ctx) => {
    await finishHookActivity(api, event, ctx, { final: false });
    return { action: "continue" };
  }, { timeoutMs: 5_000 });
  api.on("agent_end", async (event, ctx) => {
    await finishHookActivity(api, event, ctx, { final: true });
  }, { timeoutMs: 5_000 });
  api.on("after_tool_call", async (event, ctx) => {
    await attachScreenPreview(api, event, ctx);
  }, { timeoutMs: 10_000 });
}

async function startHookActivity(api, event, ctx) {
  try {
    if (!shouldAutoActivity(event, ctx)) return;
    const key = hookActivityKey(event, ctx);
    if (!key) return;
    if (!activePreviewWindows.has(key)) {
      activePreviewWindows.set(key, { startedAt: Date.now() });
    }
    if (api.pluginConfig?.auto_activity === false) return;
    if (activeRunActivities.has(key)) return;
    const correlationId = `openclaw-${sanitizeId(ctx?.runId || key).slice(0, 72)}`;
    await publishControl(await requireCredentials(), {
      schema: "weclawbot.control.v1",
      id: `activity_${crypto.randomUUID()}`,
      kind: "activity",
      activity: {
        schema: "weclawbot.activity.v1",
        state: "thinking",
        correlation_id: correlationId,
        ttl_seconds: 120,
      },
    });
    activeRunActivities.set(key, { correlationId, startedAt: Date.now() });
    api.logger?.info?.(`weclawbot activity thinking sent for ${key}`);
  } catch (error) {
    api.logger?.debug?.(`weclawbot activity hook skipped: ${errorMessage(error)}`);
  }
}

async function finishHookActivity(api, event, ctx, options = {}) {
  const key = hookActivityKey(event, ctx);
  if (!key) return;
  const previewWindow = activePreviewWindows.get(key);
  if (previewWindow) {
    try {
      await attachRecentPreviewManifests(api, event, ctx, previewWindow);
    } catch (error) {
      api.logger?.warn?.(`weclawbot preview manifest hook failed: ${errorMessage(error)}`);
    } finally {
      if (options.final) activePreviewWindows.delete(key);
    }
  }
  try {
    const active = activeRunActivities.get(key);
    if (!active) return;
    activeRunActivities.delete(key);
    await publishControl(await requireCredentials(), {
      schema: "weclawbot.control.v1",
      id: `activity_${crypto.randomUUID()}`,
      kind: "activity",
      activity: {
        schema: "weclawbot.activity.v1",
        state: "idle",
        correlation_id: active.correlationId,
      },
    });
    api.logger?.info?.(`weclawbot activity idle sent for ${key}`);
  } catch (error) {
    api.logger?.debug?.(`weclawbot activity idle hook skipped: ${errorMessage(error)}`);
  }
}

async function attachRecentPreviewManifests(api, event, ctx, previewWindow) {
  if (api.pluginConfig?.auto_preview === false) return 0;
  if (event?.error) return 0;
  const sessionKey = resolveHookSessionKey(event, ctx);
  if (!sessionKey) {
    logPreviewSkip(api, event, "missing sessionKey for manifest delivery");
    return 0;
  }
  const manifests = await listRecentPreviewManifests({
    sinceMs: Math.max(0, Number(previewWindow?.startedAt) || Date.now() - 60_000),
    untilMs: Date.now() + 10_000,
    limit: 5,
  });
  let delivered = 0;
  for (const manifest of manifests) {
    const result = await attachPreviewManifest(api, manifest, { ...ctx, sessionKey });
    if (result?.ok) {
      delivered += result.count || 1;
      await markPreviewManifest(manifest.path, {
        delivered_at: new Date().toISOString(),
        delivered_session_key: sessionKey,
        delivered_channel: result.channel,
      });
    } else {
      await markPreviewManifest(manifest.path, {
        last_delivery_error_at: new Date().toISOString(),
        last_delivery_error: result?.error || "unknown error",
      });
      api.logger?.warn?.(`weclawbot preview manifest delivery failed: ${result?.error || "unknown error"}`);
    }
  }
  return delivered;
}

async function attachPreviewManifest(api, manifest, ctx) {
  const pages = Array.isArray(manifest.preview?.pages) ? manifest.preview.pages : [];
  const files = pages
    .map((page) => ({ path: String(page.path || "") }))
    .filter((file) => file.path);
  if (files.length === 0) return { ok: false, error: "manifest has no preview files" };
  const documentId = manifest.document?.id || "screen";
  const pageLabel = `${files.length} page${files.length === 1 ? "" : "s"}`;
  const caption = `WeClawBot screen preview: ${documentId} (${pageLabel}, ${manifest.source || "manifest"})`;
  const dir = manifest.preview?.output_dir || path.dirname(files[0].path);
  return deliverPreviewFiles(api, { ctx, files, caption, dir });
}

async function attachScreenPreview(api, event, ctx) {
  try {
    if (api.pluginConfig?.auto_preview !== true) return;
    if (event?.error) return;
    const sessionKey = resolveHookSessionKey(event, ctx);
    if (!sessionKey) {
      logPreviewSkip(api, event, "missing sessionKey");
      return;
    }
    let document = null;
    let source = "tool";
    if (isScreenPublishTool(event?.toolName)) {
      document = cloneObject(event.params?.document);
      if (event.params?.force_replace) {
        document.force_replace = true;
        document.base_revision = "*";
      }
    } else if (isExecTool(event?.toolName)) {
      const file = extractScreenDocumentPathFromExecParams(event.params);
      if (!file && collectCommandStrings(event.params).some((value) => /weclawbotctl\s+screen\b/u.test(value))) {
        logPreviewSkip(api, event, "screen command detected but document path was not found");
      }
      if (!file) return;
      document = JSON.parse(await fs.readFile(file, "utf8"));
      source = "cli";
    } else {
      return;
    }
    await attachPreviewForDocument(api, document, { ...ctx, sessionKey }, source);
  } catch (error) {
    api.logger?.warn?.(`weclawbot preview attachment failed: ${errorMessage(error)}`);
  }
}

async function attachPreviewForDocument(api, document, ctx, source) {
    const validation = validateScreenDocument(document, {
      agent_transport: { available: true, screen_document_available: true },
    });
    if (!validation.ok) {
      api.logger?.warn?.(`weclawbot preview skipped: invalid screen document (${validation.errors?.[0] || "validation failed"})`);
      return;
    }
    const previewPages = renderScreenDocumentPreviewPages(document);
    if (previewPages.length === 0) {
      api.logger?.warn?.("weclawbot preview skipped: no preview pages rendered");
      return;
    }
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), "weclawbot-preview-"));
    const files = [];
    for (const page of previewPages) {
      const file = path.join(dir, `${safeFilename(document.id || "screen")}-p${page.index + 1}.png`);
      await fs.writeFile(file, page.png);
      files.push({ path: file });
    }
    const pageLabel = `${previewPages.length} page${previewPages.length === 1 ? "" : "s"}`;
    const caption = `WeClawBot screen preview: ${document.id || "screen"} (${pageLabel}, ${source})`;
    const result = await deliverPreviewFiles(api, { ctx, files, caption, dir });
    if (!result?.ok) {
      api.logger?.warn?.(`weclawbot preview delivery failed: ${result?.error || "unknown error"}`);
    } else {
      api.logger?.info?.(`weclawbot preview attached: ${result.channel} count=${result.count}`);
    }
    scheduleRemove(dir);
}

async function deliverPreviewFiles(api, params) {
    let attached = false;
    if (typeof api.session?.workflow?.sendSessionAttachment === "function") {
      const result = await api.session.workflow.sendSessionAttachment({
        sessionKey: params.ctx.sessionKey,
        files: params.files,
        text: params.caption,
        maxBytes: 2_000_000,
        captionFormat: "plain",
        channelHints: { telegram: { forceDocumentMime: "image/png" } },
      });
      if (result?.ok) {
        attached = true;
        api.logger?.info?.(`weclawbot preview attached via session workflow: ${result.channel} count=${result.count}`);
      } else {
        api.logger?.warn?.(`weclawbot session attachment unavailable: ${result?.error || "unknown error"}`);
      }
    }
    if (!attached) {
      const result = await sendPreviewViaOutboundAdapter(api, {
        ctx: params.ctx,
        files: params.files,
        caption: params.caption,
        dir: params.dir,
      });
      if (!result?.ok) {
        return result;
      } else {
        api.logger?.info?.(`weclawbot preview attached via outbound adapter: ${result.channel} count=${result.count}`);
        return result;
      }
    }
    return { ok: true, channel: "session-workflow", count: params.files.length };
}

async function sendPreviewViaOutboundAdapter(api, params) {
  const delivery = resolvePreviewDeliveryContext(api, params.ctx);
  if (!delivery?.channel || !delivery?.to) {
    return { ok: false, error: `session has no active delivery route: ${params.ctx.sessionKey}` };
  }
  const outbound = await api.runtime?.channel?.outbound?.loadAdapter?.(delivery.channel);
  if (!outbound?.sendMedia) {
    return { ok: false, error: `channel ${delivery.channel} has no media outbound adapter` };
  }
  const cfg = api.runtime?.config?.current?.() || api.config;
  let count = 0;
  for (let index = 0; index < params.files.length; index += 1) {
    const file = params.files[index];
    const suffix = params.files.length > 1 ? ` p${index + 1}/${params.files.length}` : "";
    await outbound.sendMedia({
      cfg,
      to: delivery.to,
      accountId: delivery.accountId,
      threadId: delivery.threadId,
      text: `${params.caption}${suffix}`,
      mediaUrl: file.path,
      mediaLocalRoots: [params.dir],
      mediaReadFile: async (filePath) => fs.readFile(filePath),
      forceDocument: false,
      silent: true,
    });
    count += 1;
  }
  return {
    ok: true,
    channel: delivery.channel,
    deliveredTo: delivery.to,
    count,
  };
}

function resolvePreviewDeliveryContext(api, ctx) {
  const direct = normalizeDeliveryContext(ctx?.deliveryContext);
  if (direct) return direct;
  const entry = getSessionEntryBestEffort(api, ctx);
  return normalizeDeliveryContext(entry?.deliveryContext) || normalizeDeliveryContext(entry?.route);
}

function getSessionEntryBestEffort(api, ctx) {
  const getter = api.runtime?.agent?.session?.getSessionEntry;
  if (typeof getter !== "function" || !ctx?.sessionKey) return null;
  try {
    return getter({ agentId: ctx.agentId, sessionKey: ctx.sessionKey });
  } catch {
    try {
      return getter({ sessionKey: ctx.sessionKey });
    } catch {
      return null;
    }
  }
}

function logPreviewSkip(api, event, reason) {
  api.logger?.warn?.(`weclawbot preview skipped: tool=${String(event?.toolName || "unknown")} reason=${reason}`);
}

function resolveHookSessionKey(event, ctx) {
  return String(ctx?.sessionKey || event?.sessionKey || "").trim();
}

function isScreenPublishTool(name) {
  return String(name || "").includes("weclawbot_publish_screen_document");
}

function isExecTool(name) {
  return /(^|[_-])(exec|shell|command|process)($|[_-])/iu.test(String(name || ""));
}

function shouldAutoActivity(event, ctx) {
  if (String(ctx?.trigger || "").includes("curator")) return false;
  const prompt = String(event?.prompt || "");
  if (prompt.includes("WECLAWBOT_CURATOR_EVENT")) return false;
  return SCREEN_PROMPT_PATTERN.test(prompt);
}

function hookActivityKey(event, ctx) {
  if (ctx?.runId || event?.runId) return `run:${ctx?.runId || event?.runId}`;
  if (ctx?.sessionKey) return `session:${ctx.sessionKey}`;
  return "";
}

async function requireCredentials(credentialsPath) {
  const file = expandPath(credentialsPath || DEFAULT_CREDENTIALS_PATH);
  const payload = await readCredentials(file);
  if (!payload) throw new Error(`WeClawBot is not paired. Run: weclawbotctl bind <six-digit-code>`);
  return payload;
}

async function readCredentials(file) {
  try {
    const payload = JSON.parse(await fs.readFile(file, "utf8"));
    return payload && typeof payload === "object" ? payload : null;
  } catch {
    return null;
  }
}

function expandPath(value) {
  const raw = String(value || DEFAULT_CREDENTIALS_PATH);
  return raw.startsWith("~/") ? path.join(os.homedir(), raw.slice(2)) : raw;
}

function cloneObject(value) {
  if (!value || typeof value !== "object") throw new Error("document must be an object");
  return JSON.parse(JSON.stringify(value));
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

function sanitizeId(value) {
  return String(value || crypto.randomUUID()).replace(/[^a-zA-Z0-9_.-]/gu, "_");
}

function safeFilename(value) {
  return sanitizeId(value).slice(0, 80) || "screen";
}

function scheduleRemove(dir) {
  const timer = setTimeout(() => {
    fs.rm(dir, { recursive: true, force: true }).catch(() => {});
  }, 60_000);
  timer.unref?.();
}

function errorMessage(error) {
  return String(error instanceof Error ? error.message : error).replace(/\s+/gu, " ").trim().slice(0, 240);
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
