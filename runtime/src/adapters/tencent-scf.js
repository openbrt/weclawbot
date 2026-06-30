import { curateEvent } from "../index.js";

export async function main_handler(event) {
  try {
    const request = parseRequest(event);
    const options = runtimeOptionsFromRequest(request);
    logEvent("request", {
      event_id: request.event_id || request.id || null,
      kind: request.kind || request.type || request.source?.kind || null,
      preview: previewText(request.text || request.content || ""),
      route: routeSummary(options),
    });
    const decision = await curateEvent(request, options);
    logEvent("decision", {
      event_id: decision.event_id,
      action: decision.action,
      skill: decision.skill?.id || null,
      skill_version: decision.skill?.version || null,
      has_note: Boolean(decision.note),
      has_reply: Boolean(decision.user_reply),
      trace: Array.isArray(decision.trace) ? decision.trace : undefined,
    });
    return response(200, decision);
  } catch (error) {
    logEvent("error", {
      message: error instanceof Error ? error.message : String(error),
    });
    return response(400, {
      version: 1,
      action: "service_required",
      reason: "bad_request",
      user_reply: "请求格式无效，暂不能整理成记事贴。",
      error: error instanceof Error ? error.message : String(error),
    });
  }
}

export function runtimeOptionsFromEnv(env = process.env) {
  return {
    agentFramework: env.WEC_AGENT_FRAMEWORK || "langgraph",
    includeTrace: envFlag(env, "WEC_INCLUDE_TRACE", false),
    models: {
      student: {
        enabled: envFlag(env, "STUDENT_ENABLED", false),
        provider: env.STUDENT_PROVIDER || "modelscope",
        apiKey: env.STUDENT_API_KEY || env.MODELSCOPE_API_KEY,
        baseUrl: env.STUDENT_BASE_URL || env.MODELSCOPE_BASE_URL || "https://api-inference.modelscope.cn/v1",
        modelName: env.STUDENT_MODEL || env.MODELSCOPE_MODEL || "Qwen/Qwen3.5-27B",
        minConfidence: envNumber(env, "STUDENT_MIN_CONFIDENCE", 0.82),
        timeoutMs: envNumber(env, "STUDENT_TIMEOUT_MS", 15000),
        maxTokens: envNumber(env, "STUDENT_MAX_TOKENS", 360),
        temperature: envNumber(env, "STUDENT_TEMPERATURE", 0),
        disableThinking: envFlag(env, "STUDENT_DISABLE_THINKING", false),
      },
      teacher: {
        enabled: envFlag(env, "TEACHER_ENABLED", false),
        provider: env.TEACHER_PROVIDER || "deepseek",
        apiKey: env.TEACHER_API_KEY || env.DEEPSEEK_API_KEY,
        baseUrl: env.TEACHER_BASE_URL || env.DEEPSEEK_BASE_URL || "https://api.deepseek.com",
        modelName: env.TEACHER_MODEL || env.DEEPSEEK_MODEL || "deepseek-v4-flash",
        minConfidence: envNumber(env, "TEACHER_MIN_CONFIDENCE", 0.74),
        timeoutMs: envNumber(env, "TEACHER_TIMEOUT_MS", 8000),
        maxTokens: envNumber(env, "TEACHER_MAX_TOKENS", 360),
        temperature: envNumber(env, "TEACHER_TEMPERATURE", 0),
        disableThinking: envFlag(env, "TEACHER_DISABLE_THINKING", false),
      },
    },
  };
}

export function runtimeOptionsFromRequest(request, env = process.env) {
  const options = runtimeOptionsFromEnv(env);
  const ai = request && typeof request.ai === "object" ? request.ai : null;
  if (!ai) {
    return options;
  }

  const provider = cleanString(ai.provider || ai.name || "");
  if (!provider || provider === "weclawbot") {
    return options;
  }

  const token = cleanString(ai.token || ai.api_key || ai.apiKey || ai.access_token || ai.accessToken || "");
  if (!token) {
    return options;
  }

  const endpoint = cleanString(ai.endpoint || ai.base_url || ai.baseUrl || "");
  const model = cleanString(ai.model || ai.modelName || "");
  const defaults = providerDefaults(provider);
  options.models.teacher = {
    ...(options.models.teacher || {}),
    enabled: true,
    provider,
    apiKey: token,
    baseUrl: endpoint || defaults.baseUrl,
    modelName: model || defaults.modelName,
  };
  return options;
}

function providerDefaults(provider) {
  switch (provider) {
    case "deepseek":
      return {
        baseUrl: "https://api.deepseek.com",
        modelName: "deepseek-v4-flash",
      };
    case "modelscope":
    case "qwen":
      return {
        baseUrl: "https://api-inference.modelscope.cn/v1",
        modelName: "Qwen/Qwen3.5-27B",
      };
    case "openai":
      return {
        baseUrl: "https://api.openai.com/v1",
        modelName: "gpt-4.1-mini",
      };
    default:
      return {
        baseUrl: "https://api.deepseek.com",
        modelName: "deepseek-v4-flash",
      };
  }
}

function cleanString(value) {
  return typeof value === "string" ? value.trim() : "";
}

function routeSummary(options) {
  const student = options.models?.student || {};
  const teacher = options.models?.teacher || {};
  return {
    student: student.enabled ? {
      provider: student.provider || null,
      model: student.modelName || student.model || null,
      min_confidence: student.minConfidence,
      has_key: Boolean(student.apiKey),
    } : { enabled: false },
    teacher: teacher.enabled ? {
      provider: teacher.provider || null,
      model: teacher.modelName || teacher.model || null,
      min_confidence: teacher.minConfidence,
      has_key: Boolean(teacher.apiKey),
    } : { enabled: false },
    framework: options.agentFramework || "langgraph",
  };
}

function envFlag(env, name, fallback) {
  const value = env[name];
  if (value == null || value === "") {
    return fallback;
  }
  return /^(1|true|yes|on)$/iu.test(String(value).trim());
}

function envNumber(env, name, fallback) {
  const value = env[name];
  if (value == null || value === "") {
    return fallback;
  }
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function logEvent(stage, data = {}) {
  console.log(JSON.stringify({
    level: stage === "error" ? "error" : "info",
    scope: "weclawbot-curator",
    stage,
    ts: new Date().toISOString(),
    ...data,
  }));
}

function previewText(value, max = 80) {
  const text = String(value || "").replace(/\s+/g, " ").trim();
  if (text.length <= max) {
    return text;
  }
  return `${text.slice(0, max - 1)}…`;
}

function parseRequest(event) {
  if (!event || typeof event !== "object") {
    throw new Error("SCF event must be an object");
  }
  if (typeof event.body === "string" && event.body.trim()) {
    return JSON.parse(event.body);
  }
  if (event.body && typeof event.body === "object") {
    return event.body;
  }
  return event;
}

function response(statusCode, body) {
  return {
    isBase64Encoded: false,
    statusCode,
    headers: {
      "content-type": "application/json; charset=utf-8",
    },
    body: JSON.stringify(body),
  };
}
