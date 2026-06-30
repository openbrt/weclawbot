import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Skill Library: few-shot examples that grow from teacher verdicts and
// behaviorally-grounded corrections (the tier-(a) "沉淀" layer). Injected into
// the student/teacher prompts so the cheap model inherits the expensive model's
// and the user's past judgments. No GPU, no fine-tune.
//
// The library intentionally starts empty. Each learned example is one jsonl line:
//   { "gen": 3, "source": "teacher|behavior", "grounded": true,
//     "input": { "kind": "wechat_text", "text": "…" },
//     "decision": { "action": "create_note", "note": { … } } }
//
// `grounded` marks examples confirmed by real user behavior (resend / kept /
// corrected). Teacher verdicts start ungrounded (prior, not truth) and only
// flip to grounded once behavior agrees — that is what keeps the library from
// distilling the teacher's mistakes into a cheaper echo chamber.

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_DIR = path.resolve(__dirname, "../../skills/library");

export function libraryDir(options = {}) {
  return options.dir || process.env.SKILL_LIBRARY_DIR || DEFAULT_DIR;
}

function examplesPath(skillId, options) {
  return path.join(libraryDir(options), `${skillId}.jsonl`);
}

function metaPath(skillId, options) {
  return path.join(libraryDir(options), `${skillId}.meta.json`);
}

export function readMeta(skillId, options = {}) {
  try {
    const raw = fs.readFileSync(metaPath(skillId, options), "utf8");
    const meta = JSON.parse(raw);
    return {
      generation: Number(meta.generation) || 1,
      baseline_gen: Number(meta.baseline_gen) || 0,
    };
  } catch {
    return { generation: 1, baseline_gen: 0 };
  }
}

export function loadExamples(skillId, options = {}) {
  let raw;
  try {
    raw = fs.readFileSync(examplesPath(skillId, options), "utf8");
  } catch {
    return [];
  }
  const { baseline_gen } = readMeta(skillId, options);
  const limit = Number(options.limit) || 12;
  const examples = raw
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
    .filter(Boolean)
    // MAML-style separation: drop samples authored before the last skill
    // evolution so a changed skill is never taught with stale behavior.
    .filter((ex) => (Number(ex.gen) || 1) > baseline_gen);

  // Prefer grounded, then most recent; cap to keep the prompt cheap.
  examples.sort((a, b) => {
    if (Boolean(b.grounded) !== Boolean(a.grounded)) {
      return Boolean(b.grounded) ? 1 : -1;
    }
    return (Number(b.gen) || 0) - (Number(a.gen) || 0);
  });
  return examples.slice(0, limit);
}

export function formatExamplesForPrompt(examples) {
  if (!Array.isArray(examples) || examples.length === 0) {
    return "";
  }
  const lines = examples.map((ex) => {
    const input = JSON.stringify(ex.input?.text ?? ex.input ?? "");
    const decision = JSON.stringify(compactDecision(ex.decision));
    return `- input ${input} -> ${decision}`;
  });
  return ["Learned examples from confirmed behavior (follow these patterns):", ...lines].join("\n");
}

function compactDecision(decision = {}) {
  const out = { action: decision.action };
  if (decision.note) {
    out.note = {
      title: decision.note.title,
      body: decision.note.body,
      priority: decision.note.priority || "normal",
    };
  }
  return out;
}

export function appendExample(skillId, example, options = {}) {
  const dir = libraryDir(options);
  fs.mkdirSync(dir, { recursive: true });
  const { generation } = readMeta(skillId, options);
  const record = {
    gen: example.gen ?? generation,
    source: example.source || "behavior",
    grounded: Boolean(example.grounded),
    input: example.input,
    decision: compactDecision(example.decision || {}),
    ts: new Date().toISOString(),
  };
  fs.appendFileSync(examplesPath(skillId, options), `${JSON.stringify(record)}\n`, "utf8");
  return record;
}

// Called when a rule has been precipitated into sticky-core (skill evolved):
// bump the generation and raise the baseline so the next eval/library load only
// uses post-evolution samples.
export function evolveSkill(skillId, options = {}) {
  const dir = libraryDir(options);
  fs.mkdirSync(dir, { recursive: true });
  const meta = readMeta(skillId, options);
  const next = {
    generation: meta.generation + 1,
    baseline_gen: meta.generation,
    evolved_at: new Date().toISOString(),
  };
  fs.writeFileSync(metaPath(skillId, options), `${JSON.stringify(next, null, 2)}\n`, "utf8");
  return next;
}
