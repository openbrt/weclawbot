import { curateWithChatModel } from "../models/chat-curator.js";
import { recordTeacherVerdict } from "./precipitation.js";

const DISPLAY_WRAP_UNITS = 44;
const DISPLAY_LINES_PER_PAGE = 10;
const MAX_DISPLAY_PAGES = 3;

// Confidence cascade: rules -> student (free) -> teacher (paid DeepSeek).
// Each tier is only invoked when the previous decision is not confident enough.
// The teacher's verdict is recorded as a precipitation candidate (a prior, not
// truth) so the Skill Library can grow once user behavior confirms it.
//
// Back-compat: a single `options.model` config is treated as a lone teacher tier
// (the previous behavior). When `options.models` is absent and no model is
// configured, rules pass straight through (eval stays offline / deterministic).

export async function maybeRouteToModel(bundle, ruleDecision, options = {}) {
  const tiers = resolveTiers(options);
  if (tiers.length === 0) {
    return ruleDecision;
  }

  let decision = ruleDecision;
  let lastTierDecision = null; // decision feeding the current tier (e.g. student before teacher)

  for (const [index, tier] of tiers.entries()) {
    if (!shouldEscalate(decision, tier)) {
      break;
    }
    const prior = decision;
    try {
      const next = await runTier(tier, bundle, options);
      const guarded = guardTierDecision(tier, prior, next);
      if (guarded.rejected) {
        lastTierDecision = next;
        decision = guarded.decision;
        continue;
      }
      if (tier.role === "teacher") {
        // student (or rules) -> teacher: candidate for the Skill Library
        recordTeacherVerdict(bundle, lastTierDecision || prior, next, options);
      }
      lastTierDecision = next;
      decision = adoptTierDecision(prior, next, tier.role);
    } catch (error) {
      decision = withTrace(decision, `tier_failed:${tier.role}:${errMsg(error)}`);
      if (tier.role !== "teacher" && tiers.slice(index + 1).some((next) => next.role === "teacher")) {
        continue;
      }
      break;
    }
  }

  return decision;
}

function guardTierDecision(tier, prior, next) {
  if (tier.role !== "student") {
    return { rejected: false, decision: next };
  }
  if (prior?.action === "clarify" && next?.action === "ignore") {
    return {
      rejected: true,
      decision: {
        ...prior,
        confidence: Math.max(Number(prior.confidence || 0), tier.minConfidence),
        trace: [
          ...(prior.trace || []),
          ...(next.trace || []),
          `tier_rejected:${tier.role}:clarify_to_ignore`,
        ],
      },
    };
  }
  if (prior?.action !== "create_note" || isDisplayAction(next?.action)) {
    if (isDisplayAction(next?.action) && !fitsDisplayBudget(next.note)) {
      return {
        rejected: true,
        decision: {
          ...prior,
          confidence: Math.min(Number(prior.confidence || 0), 0.5),
          trace: [
            ...(prior.trace || []),
            ...(next.trace || []),
            `tier_rejected:${tier.role}:display_overflow`,
          ],
        },
      };
    }
    return { rejected: false, decision: next };
  }
  return {
    rejected: true,
    decision: {
      ...prior,
      confidence: Math.min(Number(prior.confidence || 0), 0.5),
      trace: [
        ...(prior.trace || []),
        ...(next.trace || []),
        `tier_rejected:${tier.role}:note_downgrade`,
      ],
    },
  };
}

function isDisplayAction(action) {
  return action === "create_note" || action === "update_note" ||
    action === "replace_note" || action === "merge_note" || action === "draft_note";
}

function fitsDisplayBudget(note) {
  if (!note) {
    return true;
  }
  const title = cleanLine(note.title || "");
  const body = cleanLine(note.body || note.content || "");
  const text = title && title !== "记事" && body && !body.includes(title)
    ? `${title}\n${body}`
    : (body || title);
  return wrappedLineCount(text) <= DISPLAY_LINES_PER_PAGE * MAX_DISPLAY_PAGES;
}

function wrappedLineCount(text) {
  const lines = String(text || "")
    .replace(/\r/g, "\n")
    .split("\n")
    .map(cleanLine)
    .filter(Boolean);
  let count = 0;
  for (const line of lines.length ? lines : [""]) {
    count += Math.max(1, Math.ceil(visualLineUnits(line) / DISPLAY_WRAP_UNITS));
  }
  return count;
}

function cleanLine(value) {
  return String(value || "").replace(/\s+/gu, " ").trim();
}

function visualLineUnits(value) {
  return [...String(value || "")].reduce((sum, ch) => sum + visualUnits(ch), 0);
}

function visualUnits(ch) {
  if (ch === " ") {
    return 0.55;
  }
  if (/[\u0021-\u002f\u003a-\u0040\u005b-\u0060\u007b-\u007e]/u.test(ch)) {
    return 0.6;
  }
  if (/[A-Z0-9]/u.test(ch)) {
    return 0.95;
  }
  if (/[a-z]/u.test(ch)) {
    return 0.85;
  }
  return ch.charCodeAt(0) < 0x80 ? 1 : 2;
}

function resolveTiers(options) {
  const tiers = [];
  const models = options.models || {};

  const student = models.student || options.student;
  if (student && student.enabled) {
    tiers.push({
      role: "student",
      minConfidence: numberOr(student.minConfidence, 0.82),
      always: Boolean(student.always),
      config: student,
    });
  }

  // New explicit teacher, or legacy single `options.model`.
  const teacher = models.teacher || options.teacher || options.model;
  if (teacher && teacher.enabled) {
    tiers.push({
      role: "teacher",
      minConfidence: numberOr(teacher.minConfidence ?? teacher.minRuleConfidence, student?.enabled ? 0.7 : 0.82),
      always: Boolean(teacher.always),
      config: teacher,
    });
  }

  return tiers;
}

function shouldEscalate(decision, tier) {
  if (tier.always) {
    return true;
  }
  // Capability misses (service_required) are deterministic; no model can fix a
  // missing extractor, so don't spend a tier on them.
  if (decision.action === "service_required") {
    return false;
  }
  return Number(decision.confidence || 0) < tier.minConfidence;
}

async function runTier(tier, bundle, options) {
  const cfg = tier.config || {};
  return curateWithChatModel(bundle, {
    role: tier.role,
    apiKey: cfg.apiKey || (tier.role === "teacher"
      ? process.env.DEEPSEEK_API_KEY
      : (process.env.STUDENT_API_KEY || process.env.MODELSCOPE_API_KEY)),
    baseUrl: cfg.baseUrl || (tier.role === "teacher"
      ? (process.env.DEEPSEEK_BASE_URL || "https://api.deepseek.com")
      : (process.env.STUDENT_BASE_URL || process.env.MODELSCOPE_BASE_URL || "https://api-inference.modelscope.cn/v1")),
    modelName: cfg.modelName || cfg.model
      || (tier.role === "teacher"
        ? process.env.DEEPSEEK_MODEL
        : (process.env.STUDENT_MODEL || process.env.MODELSCOPE_MODEL || "Qwen/Qwen3.5-27B")),
    maxTokens: cfg.maxTokens ?? tierEnvNumber(tier, "MAX_TOKENS", undefined),
    timeoutMs: cfg.timeoutMs ?? tierEnvNumber(tier, "TIMEOUT_MS", undefined),
    temperature: cfg.temperature ?? tierEnvNumber(tier, "TEMPERATURE", undefined),
    disableThinking: cfg.disableThinking ?? tierEnvFlag(tier, "DISABLE_THINKING", undefined),
    skillId: options.skillId || cfg.skillId,
    skillLibrary: cfg.skillLibrary,
  });
}

function withTrace(decision, entry) {
  return {
    ...decision,
    trace: [...(decision.trace || []), entry],
  };
}

function adoptTierDecision(prior, next, tierRole) {
  return {
    ...next,
    trace: [
      ...(prior.trace || []),
      ...(next.trace || []),
      `tier:${tierRole}`,
    ],
  };
}

function numberOr(value, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function tierEnvNumber(tier, suffix, fallback) {
  const primary = tier.role === "teacher" ? `TEACHER_${suffix}` : `STUDENT_${suffix}`;
  const value = process.env[primary];
  if (value == null || value === "") {
    return fallback;
  }
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function tierEnvFlag(tier, suffix, fallback) {
  const primary = tier.role === "teacher" ? `TEACHER_${suffix}` : `STUDENT_${suffix}`;
  const value = process.env[primary];
  if (value == null || value === "") {
    return fallback;
  }
  return /^(1|true|yes|on)$/iu.test(String(value).trim());
}

function errMsg(error) {
  return error instanceof Error ? error.message : String(error);
}
