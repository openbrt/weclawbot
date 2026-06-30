const TEXT_KINDS = new Set([
  "wechat_text",
  "wechat_voice_transcript",
  "text",
  "voice_transcript",
]);

const FILE_KINDS = new Set([
  "wechat_image",
  "wechat_file",
  "image",
  "pdf",
  "docx",
  "pptx",
  "xlsx",
  "csv",
  "raw_voice",
  "unknown_file",
]);

export function eventToBundle(event) {
  if (!event || typeof event !== "object") {
    throw new Error("event must be an object");
  }

  if (Array.isArray(event.blocks)) {
    return normalizeBundle(event);
  }

  const source = normalizeSource(event);
  const text = extractText(event, source.kind);
  const blocks = [];

  if (text) {
    blocks.push({
      type: "text",
      text,
    });
  }

  if (FILE_KINDS.has(source.kind) && event.filename) {
    blocks.push({
      type: "metadata",
      key: "filename",
      text: event.filename,
    });
  }

  if (FILE_KINDS.has(source.kind) && event.media && typeof event.media === "object") {
    const media = event.media;
    if (media.key_type || media.mime_type || media.byte_size) {
      blocks.push({
        type: "metadata",
        key: "media",
        text: [
          media.key_type ? `key_type=${media.key_type}` : "",
          media.mime_type ? `mime_type=${media.mime_type}` : "",
          media.byte_size ? `bytes=${media.byte_size}` : "",
        ].filter(Boolean).join(" "),
      });
    }
    if (media.url || media.cdn_url) {
      blocks.push({
        type: "metadata",
        key: "media_url",
        text: "present",
      });
    }
    if (media.aes_key || media.aeskey) {
      blocks.push({
        type: "metadata",
        key: "media_key",
        text: "present",
      });
    }
  }

  return normalizeBundle({
    version: 1,
    event_id: event.event_id || event.id || stableEventId(source, text || event.filename || ""),
    received_at: event.received_at,
    source,
    screen: event.screen || event.current_screen || event.context?.screen,
    blocks,
  });
}

function normalizeSource(event) {
  const incoming = event.source && typeof event.source === "object" ? event.source : {};
  const kind = incoming.kind || event.kind || event.type || "wechat_text";
  return {
    kind,
    filename: incoming.filename || event.filename || "",
    sender_ref: incoming.sender_ref || event.sender_ref || "",
  };
}

function extractText(event, kind) {
  if (typeof event.text === "string") {
    return event.text.trim();
  }
  if (typeof event.content === "string") {
    return event.content.trim();
  }
  if (kind === "wechat_voice_transcript" && event.voice_item && typeof event.voice_item.text === "string") {
    return event.voice_item.text.trim();
  }
  return "";
}

function normalizeBundle(bundle) {
  const source = bundle.source && typeof bundle.source === "object" ? bundle.source : {};
  const blocks = Array.isArray(bundle.blocks) ? bundle.blocks.map(normalizeBlock).filter(Boolean) : [];
  return {
    version: 1,
    event_id: bundle.event_id || bundle.id || stableEventId(source, JSON.stringify(blocks)),
    received_at: bundle.received_at || new Date().toISOString(),
    source: {
      kind: source.kind || "unknown",
      filename: source.filename || "",
      sender_ref: source.sender_ref || "",
    },
    screen: normalizeScreen(bundle.screen || bundle.current_screen || bundle.context?.screen),
    blocks,
  };
}

function normalizeScreen(screen) {
  if (!screen || typeof screen !== "object") {
    return {
      has_note: false,
      current_note: null,
    };
  }
  const current = normalizeCurrentNote(screen.current_note || screen.note || screen.current);
  return {
    version: Number.isFinite(Number(screen.version)) ? Number(screen.version) : 1,
    has_note: Boolean(screen.has_note || current),
    note_count: Number.isFinite(Number(screen.note_count)) ? Number(screen.note_count) : (current ? 1 : 0),
    note_index: Number.isFinite(Number(screen.note_index)) ? Number(screen.note_index) : 0,
    current_note: current,
  };
}

function normalizeCurrentNote(note) {
  if (!note || typeof note !== "object") {
    return null;
  }
  const text = typeof note.text === "string" ? note.text.trim() : "";
  const canonicalText = typeof note.canonical_text === "string" ? note.canonical_text.trim() : "";
  const title = typeof note.title === "string" ? note.title.trim() : "";
  const body = typeof note.body === "string" ? note.body.trim() : "";
  const content = canonicalText || text || [title, body].filter(Boolean).join("\n").trim();
  if (!content) {
    return null;
  }
  return {
    text: content.slice(0, 800),
    canonical_text: content.slice(0, 800),
    title: title.slice(0, 80),
    body: body.slice(0, 800),
    time_label: typeof note.time_label === "string" ? note.time_label.trim().slice(0, 40) : "",
    created_at: typeof note.created_at === "number" ? note.created_at : undefined,
    revision: typeof note.revision === "string" ? note.revision.trim().slice(0, 80) : "",
    render_id: typeof note.render_id === "string" ? note.render_id.trim().slice(0, 96) : "",
    page_count: Number.isFinite(Number(note.page_count)) ? Number(note.page_count) : 0,
  };
}

function normalizeBlock(block) {
  if (!block || typeof block !== "object") {
    return null;
  }
  const text = typeof block.text === "string" ? block.text.trim() : "";
  if (!text && block.type !== "table") {
    return null;
  }
  return {
    ...block,
    type: block.type || "text",
    text,
  };
}

function stableEventId(source, text) {
  const raw = `${source.kind || "unknown"}:${source.sender_ref || ""}:${text}`;
  let hash = 2166136261;
  for (let i = 0; i < raw.length; i += 1) {
    hash ^= raw.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return `local_${(hash >>> 0).toString(16).padStart(8, "0")}`;
}
