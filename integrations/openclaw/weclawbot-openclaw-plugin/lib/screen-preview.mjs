import crypto from "node:crypto";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { deflateSync } from "node:zlib";

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

export function renderScreenDocumentPreviewPages(document, options = {}) {
  const pages = Array.isArray(document?.pages) ? document.pages : [];
  const scale = clampInteger(options.scale ?? 2, 1, 4);
  return pages.map((page, index) => {
    const png = renderMonoPagePreview(page, { scale });
    return {
      index,
      width: Number(page.width) * scale,
      height: Number(page.height) * scale,
      scale,
      png,
      bytes: png.length,
      sha256: crypto.createHash("sha256").update(png).digest("hex"),
    };
  });
}

export function previewSummary(previewPages) {
  return {
    available: previewPages.length > 0,
    pages: previewPages.map((page) => ({
      index: page.index,
      width: page.width,
      height: page.height,
      scale: page.scale,
      bytes: page.bytes,
      sha256: page.sha256,
      mime_type: "image/png",
    })),
  };
}

export async function writeScreenDocumentPreviewFiles(document, options = {}) {
  const previewPages = renderScreenDocumentPreviewPages(document, options);
  const outputDir = options.outputDir
    ? expandPath(options.outputDir)
    : await fs.mkdtemp(path.join(os.tmpdir(), "weclawbot-preview-"));
  await fs.mkdir(outputDir, { recursive: true });
  const basename = safeFilename(options.basename || document?.id || "screen");
  const pages = [];
  for (const page of previewPages) {
    const file = path.join(outputDir, `${basename}-p${page.index + 1}.png`);
    await fs.writeFile(file, page.png);
    pages.push({
      index: page.index,
      path: file,
      width: page.width,
      height: page.height,
      scale: page.scale,
      bytes: page.bytes,
      sha256: page.sha256,
      mime_type: "image/png",
    });
  }
  return {
    available: pages.length > 0,
    output_dir: outputDir,
    pages,
  };
}

function renderMonoPagePreview(page, options = {}) {
  const width = Number(page?.width);
  const height = Number(page?.height);
  const stride = Number(page?.stride);
  const scale = clampInteger(options.scale ?? 2, 1, 4);
  if (!Number.isInteger(width) || width < 1 || !Number.isInteger(height) || height < 1) {
    throw new Error("preview page width/height must be positive integers");
  }
  if (!Number.isInteger(stride) || stride < Math.ceil(width / 8)) {
    throw new Error("preview page stride is invalid");
  }
  const packed = Buffer.from(String(page?.data_b64 || ""), "base64");
  if (packed.length < stride * height) {
    throw new Error("preview page data is shorter than stride * height");
  }

  const outWidth = width * scale;
  const outHeight = height * scale;
  const gray = Buffer.alloc(outWidth * outHeight, 0xff);
  for (let y = 0; y < height; y += 1) {
    const row = y * stride;
    for (let x = 0; x < width; x += 1) {
      const black = (packed[row + (x >> 3)] & (0x80 >> (x & 7))) !== 0;
      if (!black) continue;
      for (let dy = 0; dy < scale; dy += 1) {
        const outRow = (y * scale + dy) * outWidth;
        for (let dx = 0; dx < scale; dx += 1) {
          gray[outRow + x * scale + dx] = 0;
        }
      }
    }
  }
  return encodeGrayscalePng(gray, outWidth, outHeight);
}

function encodeGrayscalePng(gray, width, height) {
  const raw = Buffer.alloc((width + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const row = y * (width + 1);
    raw[row] = 0;
    gray.copy(raw, row + 1, y * width, (y + 1) * width);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 0;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  return Buffer.concat([
    PNG_SIGNATURE,
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", deflateSync(raw)),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
}

function pngChunk(type, data) {
  const typeBuf = Buffer.from(type, "ascii");
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

const crcTable = new Uint32Array(256).map((_, index) => {
  let c = index;
  for (let bit = 0; bit < 8; bit += 1) {
    c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
  }
  return c >>> 0;
});

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) {
    crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function clampInteger(value, min, max) {
  const parsed = Number.parseInt(String(value), 10);
  if (!Number.isFinite(parsed)) return min;
  return Math.max(min, Math.min(max, parsed));
}

function expandPath(value) {
  const raw = String(value || "");
  return raw.startsWith("~/") ? path.join(os.homedir(), raw.slice(2)) : raw;
}

function safeFilename(value) {
  return String(value || "screen").replace(/[^a-zA-Z0-9_.-]/gu, "_").slice(0, 80) || "screen";
}
