import crypto from "node:crypto";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const MANIFEST_SCHEMA = "weclawbot.preview_manifest.v1";

export async function defaultPreviewManifestRoot() {
  const override = expandPath(process.env.WECLAWBOT_PREVIEW_SPOOL_DIR || "");
  if (override) return override;

  const stateDir = expandPath(process.env.OPENCLAW_STATE_DIR || "");
  if (stateDir) return path.join(stateDir, "media", "outbound", "weclawbot-preview");

  const openClawOutbound = path.join(os.homedir(), ".openclaw", "media", "outbound");
  if (await isDirectory(openClawOutbound)) return path.join(openClawOutbound, "weclawbot-preview");

  return path.join(os.homedir(), ".cache", "weclawbot", "openclaw-preview");
}

export async function createPreviewArtifactDir(documentId = "screen") {
  const root = await defaultPreviewManifestRoot();
  const now = new Date();
  const stamp = now.toISOString().replace(/[:.]/gu, "-");
  const dir = path.join(root, "files", `${stamp}-${process.pid}-${safeFilename(documentId)}`);
  await fs.mkdir(dir, { recursive: true });
  return dir;
}

export async function writePreviewManifest({ document, preview, source = "unknown", status = {}, createdAtMs = Date.now() }) {
  if (!preview?.available || !Array.isArray(preview.pages) || preview.pages.length === 0) return null;
  const root = await defaultPreviewManifestRoot();
  const dir = path.join(root, "manifests");
  await fs.mkdir(dir, { recursive: true });

  const id = `preview_${crypto.randomUUID()}`;
  const createdAt = new Date(createdAtMs).toISOString();
  const manifest = {
    schema: MANIFEST_SCHEMA,
    id,
    created_at: createdAt,
    created_ms: createdAtMs,
    source,
    document: {
      id: String(document?.id || "screen"),
      pages: Array.isArray(document?.pages) ? document.pages.length : preview.pages.length,
    },
    status,
    preview: {
      available: true,
      output_dir: preview.output_dir || path.dirname(preview.pages[0]?.path || ""),
      pages: preview.pages.map((page) => ({
        index: Number(page.index) || 0,
        path: String(page.path || ""),
        width: Number(page.width) || 0,
        height: Number(page.height) || 0,
        scale: Number(page.scale) || 1,
        bytes: Number(page.bytes) || 0,
        sha256: String(page.sha256 || ""),
        mime_type: page.mime_type || "image/png",
      })),
    },
  };

  const file = path.join(dir, `${createdAtMs}-${safeFilename(document?.id || source)}-${id}.json`);
  await fs.writeFile(file, `${JSON.stringify(manifest, null, 2)}\n`, { mode: 0o600 });
  return { ...manifest, path: file, root };
}

export async function listRecentPreviewManifests(options = {}) {
  const root = await defaultPreviewManifestRoot();
  const dir = path.join(root, "manifests");
  const sinceMs = Number(options.sinceMs) || 0;
  const untilMs = Number(options.untilMs) || Date.now() + 10_000;
  const limit = Math.max(1, Number(options.limit) || 8);
  let entries = [];
  try {
    entries = await fs.readdir(dir, { withFileTypes: true });
  } catch (error) {
    if (error?.code === "ENOENT") return [];
    throw error;
  }

  const manifests = [];
  for (const entry of entries) {
    if (!entry.isFile() || !entry.name.endsWith(".json")) continue;
    const file = path.join(dir, entry.name);
    let stat;
    let manifest;
    try {
      stat = await fs.stat(file);
      if (stat.mtimeMs < sinceMs - 10_000 || stat.mtimeMs > untilMs + 10_000) continue;
      manifest = JSON.parse(await fs.readFile(file, "utf8"));
    } catch {
      continue;
    }
    if (manifest?.schema !== MANIFEST_SCHEMA) continue;
    if (manifest.delivered_at || manifest.consumed_at) continue;
    const createdMs = Number(manifest.created_ms) || Date.parse(manifest.created_at || "") || stat.mtimeMs;
    if (createdMs < sinceMs || createdMs > untilMs) continue;
    const pages = Array.isArray(manifest.preview?.pages) ? manifest.preview.pages : [];
    if (pages.length === 0) continue;
    manifests.push({ ...manifest, created_ms: createdMs, path: file, root });
  }

  return manifests
    .sort((a, b) => a.created_ms - b.created_ms)
    .slice(0, limit);
}

export async function markPreviewManifest(file, patch) {
  const payload = JSON.parse(await fs.readFile(file, "utf8"));
  const updated = { ...payload, ...patch };
  await fs.writeFile(file, `${JSON.stringify(updated, null, 2)}\n`, { mode: 0o600 });
  return updated;
}

async function isDirectory(file) {
  try {
    return (await fs.stat(file)).isDirectory();
  } catch {
    return false;
  }
}

function expandPath(value) {
  const raw = String(value || "").trim();
  return raw.startsWith("~/") ? path.join(os.homedir(), raw.slice(2)) : raw;
}

function safeFilename(value) {
  return String(value || "screen").replace(/[^a-zA-Z0-9_.-]/gu, "_").slice(0, 80) || "screen";
}
