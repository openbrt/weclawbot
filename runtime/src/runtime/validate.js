const ACTIONS = new Set([
  "ignore",
  "create_note",
  "update_note",
  "replace_note",
  "merge_note",
  "clear_note",
  "clarify",
  "reply_only",
  "draft_note",
  "service_required",
]);

const PRIORITIES = new Set(["low", "normal", "high"]);

export function validateDecision(decision, options = {}) {
  if (!decision || typeof decision !== "object") {
    throw new Error("decision must be an object");
  }
  if (!ACTIONS.has(decision.action)) {
    throw new Error(`unsupported action: ${decision.action}`);
  }

  const normalized = {
    version: 1,
    event_id: requiredString(decision.event_id, "event_id"),
    action: decision.action,
    confidence: clampConfidence(decision.confidence),
    skill: decision.skill || null,
  };

  if (decision.action === "create_note" || decision.action === "update_note" ||
      decision.action === "replace_note" || decision.action === "merge_note" ||
      decision.action === "draft_note") {
    normalized.note = validateNote(decision.note || {});
  }

  if (decision.action === "clarify" || decision.action === "reply_only") {
    normalized.user_reply = boundedText(requiredString(decision.user_reply, "user_reply"), 160);
  }

  if (decision.action === "service_required") {
    normalized.user_reply = boundedText(decision.user_reply || "当前需要外部处理能力，暂不能整理成记事贴。", 160);
    normalized.reason = boundedText(decision.reason || "runtime_capability_missing", 64);
  }

  if (!normalized.user_reply && typeof decision.user_reply === "string" && decision.user_reply.trim()) {
    normalized.user_reply = boundedText(decision.user_reply, 220);
  }

  if (normalized.note &&
      (decision.action === "create_note" || decision.action === "update_note" ||
       decision.action === "replace_note" || decision.action === "merge_note")) {
    const reply = typeof decision.user_reply === "string" ? decision.user_reply.trim() : "";
    if (!reply || !/(屏幕|微笺)/u.test(reply)) {
      normalized.user_reply = boundedText(
        `已覆盖到屏幕：${normalized.note.body}。不合适可直接发“修改为…”或“清屏”。`,
        220,
      );
    } else {
      normalized.user_reply = boundedText(reply.replace(/便签/g, "微笺"), 220);
    }
  }

  if (options.includeTrace && decision.trace) {
    normalized.trace = decision.trace;
  }

  return normalized;
}

function validateNote(note) {
  const body = boundedText(requiredString(note.body || note.content, "note.body"), 520, {
    preserveNewlines: true,
  });
  return {
    template: note.template || "sticky.v1",
    title: boundedText(note.title || "记事", 18),
    body,
    footer: boundedText(note.footer || "", 24),
    priority: PRIORITIES.has(note.priority) ? note.priority : "normal",
    expires_at: typeof note.expires_at === "string" ? note.expires_at : undefined,
  };
}

function requiredString(value, field) {
  if (typeof value !== "string" || value.trim() === "") {
    throw new Error(`${field} is required`);
  }
  return value.trim();
}

function boundedText(value, max, options = {}) {
  const text = options.preserveNewlines
    ? String(value || "")
      .replace(/\r/g, "\n")
      .replace(/[ \t\f\v]+/g, " ")
      .replace(/[ \t]*\n[ \t]*/g, "\n")
      .replace(/\n{3,}/g, "\n\n")
      .trim()
    : String(value || "").trim().replace(/\s+/g, " ");
  if (text.length <= max) {
    return text;
  }
  return `${text.slice(0, Math.max(0, max - 1)).trim()}…`;
}

function clampConfidence(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) {
    return 0.5;
  }
  return Math.max(0, Math.min(1, n));
}
