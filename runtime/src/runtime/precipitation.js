import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { appendExample, readMeta } from "./skill-library.js";

// Precipitation: how teacher verdicts and real user behavior turn into Skill
// Library examples. The load-bearing rule is that the *teacher is a prior, not
// truth*. A teacher verdict alone never enters the library; it waits in a
// candidate ledger until a behavioral signal (the user kept / resent / corrected
// the note) agrees with it. User behavior is the reward — never an LLM judge —
// which is what stops the cascade from distilling the teacher's mistakes.
//
// Two ledgers (jsonl), written off the hot path and tolerant of read-only fs:
//   teacher-candidates.jsonl  — teacher disagreed with student/rules (ungrounded)
//   gap-events.jsonl          — behavioral truth: resend | clear | correction

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_DIR = path.resolve(__dirname, "../../skills/precipitation");

export function precipitationDir(options = {}) {
  return options.precipitationDir || process.env.PRECIPITATION_DIR || DEFAULT_DIR;
}

function ledgerPath(name, options) {
  return path.join(precipitationDir(options), name);
}

function appendLine(name, record, options) {
  try {
    const dir = precipitationDir(options);
    fs.mkdirSync(dir, { recursive: true });
    fs.appendFileSync(ledgerPath(name, options), `${JSON.stringify(record)}\n`, "utf8");
    return true;
  } catch {
    // Hot path must never throw on a read-only or full filesystem.
    return false;
  }
}

function readLines(name, options) {
  try {
    return fs.readFileSync(ledgerPath(name, options), "utf8")
      .split("\n")
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => {
        try {
          return JSON.parse(line);
        } catch {
          return null;
        }
      })
      .filter(Boolean);
  } catch {
    return [];
  }
}

// Hot path (cascade): record teacher verdicts that DISAGREE with the prior
// decision. Agreement is not a learning signal. Stored ungrounded.
export function recordTeacherVerdict(bundle, priorDecision, teacherDecision, options = {}) {
  if (!teacherDecision || teacherDecision.action === priorDecision?.action) {
    return false;
  }
  const skillId = options.skillId || "sticky-core";
  return appendLine("teacher-candidates.jsonl", {
    ts: new Date().toISOString(),
    event_id: bundle.event_id,
    skill_id: skillId,
    gen: readMeta(skillId, options).generation,
    input: { kind: bundle.source?.kind, text: bundleText(bundle) },
    prior_action: priorDecision?.action || null,
    teacher: { action: teacherDecision.action, note: teacherDecision.note || null },
    grounded: false,
  }, options);
}

// Off-device (proxy) calls this when the user's behavior reveals truth about a
// past decision. `type`: "false_negative" (resend), "false_positive" (rapid
// clear), "content" / "render" (correction). `truth` optionally carries the
// corrected action/note the user implied.
export function recordGapEvent(gap, options = {}) {
  const skillId = gap.skill_id || options.skillId || "sticky-core";
  return appendLine("gap-events.jsonl", {
    ts: new Date().toISOString(),
    event_id: gap.event_id || null,
    skill_id: skillId,
    type: gap.type,
    input: gap.input || null,
    observed_action: gap.observed_action || null,
    truth: gap.truth || null,
    promoted: false,
  }, options);
}

// Idle-window job (precipitate.mjs): turn grounded signals into Skill Library
// examples + eval cases. A teacher candidate is promoted only if a gap event
// agrees with it; a correction gap with an explicit truth promotes on its own.
export function precipitate(options = {}) {
  const skillId = options.skillId || "sticky-core";
  const candidates = readLines("teacher-candidates.jsonl", options)
    .filter((c) => c.skill_id === skillId && !c.grounded);
  const gaps = readLines("gap-events.jsonl", options)
    .filter((g) => g.skill_id === skillId && !g.promoted);

  const gapByEvent = new Map();
  for (const gap of gaps) {
    if (gap.event_id) {
      gapByEvent.set(gap.event_id, gap);
    }
  }

  const promoted = [];

  // 1) Behavioral corrections with explicit truth: promote directly (grounded).
  for (const gap of gaps) {
    if (gap.truth && gap.truth.action) {
      const example = appendExample(skillId, {
        source: "behavior",
        grounded: true,
        input: gap.input,
        decision: gap.truth,
      }, options);
      promoted.push({ via: `gap:${gap.type}`, example });
    }
  }

  // 2) Teacher candidates confirmed by a behavioral signal on the same event.
  for (const candidate of candidates) {
    const gap = candidate.event_id ? gapByEvent.get(candidate.event_id) : null;
    if (!gap) {
      continue;
    }
    // Confirmation: behavior wanted a note (false_negative / content) and the
    // teacher proposed one — the teacher's prior is now grounded.
    const behaviorWantsNote = gap.type === "false_negative" || gap.type === "content";
    if (behaviorWantsNote && candidate.teacher.action === "create_note") {
      const example = appendExample(skillId, {
        source: "teacher",
        grounded: true,
        input: candidate.input,
        decision: candidate.teacher,
      }, options);
      promoted.push({ via: "teacher+behavior", example });
    }
  }

  return { skillId, promoted: promoted.length, items: promoted };
}

function bundleText(bundle) {
  if (!bundle || !Array.isArray(bundle.blocks)) {
    return "";
  }
  return bundle.blocks.map((b) => b.text || "").filter(Boolean).join("\n").slice(0, 400);
}
