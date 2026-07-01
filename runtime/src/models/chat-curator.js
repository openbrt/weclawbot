import { validateDecision } from "../runtime/validate.js";
import { loadExamples, formatExamplesForPrompt } from "../runtime/skill-library.js";

// Generic OpenAI-compatible curator call shared by both cascade tiers
// (free "student" and paid "teacher"). The only difference between tiers is the
// endpoint/model/key passed in options; both inherit the same Skill Library
// few-shot examples so the cheap student keeps catching up to the teacher.

const DEFAULT_FALLBACK_CONFIDENCE = 0.78;

export async function curateWithChatModel(bundle, options = {}) {
  const apiKey = options.apiKey;
  if (!apiKey) {
    throw new Error(`${options.role || "model"}_api_key_missing`);
  }
  const baseUrl = String(options.baseUrl || "https://api.deepseek.com").replace(/\/+$/u, "");
  const model = options.modelName || options.model || "deepseek-v4-flash";

  const examples = options.skillLibrary === false
    ? []
    : (options.examples || loadExamples(options.skillId || "sticky-core", options));

  const body = {
    model,
    messages: [
      { role: "system", content: systemPrompt(examples) },
      { role: "user", content: JSON.stringify(bundle) },
    ],
    response_format: { type: "json_object" },
    stream: false,
    max_tokens: options.maxTokens || 360,
    temperature: options.temperature ?? 0,
  };
  if (options.disableThinking === true) {
    body.thinking = { type: "disabled" };
  }

  const content = await requestChatContent(baseUrl, apiKey, body, options);

  let parsed;
  try {
    parsed = parseModelJson(content);
  } catch {
    throw new Error(`${options.role || "model"}_invalid_json`);
  }

  return validateDecision({
    version: 1,
    event_id: bundle.event_id,
    ...parsed,
    confidence: parsed.confidence ?? DEFAULT_FALLBACK_CONFIDENCE,
    trace: [`${options.role || "model"}_decision`],
  }, { includeTrace: true });
}

async function requestChatContent(baseUrl, apiKey, body, options) {
  const role = options.role || "model";
  const response = await fetch(`${baseUrl}/chat/completions`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      authorization: `Bearer ${apiKey}`,
    },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(options.timeoutMs || 8000),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(`${role}_http_${response.status}:${errorText.slice(0, 160)}`);
  }

  const data = await response.json();
  const content = data?.choices?.[0]?.message?.content;
  if (typeof content === "string" && content.trim()) {
    return content;
  }

  if (options.streamFallback === false) {
    throw new Error(`${role}_empty_content`);
  }

  const streamed = await requestStreamContent(baseUrl, apiKey, body, options);
  if (!streamed.trim()) {
    throw new Error(`${role}_empty_content`);
  }
  return streamed;
}

async function requestStreamContent(baseUrl, apiKey, body, options) {
  const role = options.role || "model";
  const response = await fetch(`${baseUrl}/chat/completions`, {
    method: "POST",
    headers: {
      "content-type": "application/json",
      authorization: `Bearer ${apiKey}`,
    },
    body: JSON.stringify({ ...body, stream: true }),
    signal: AbortSignal.timeout(options.timeoutMs || 8000),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(`${role}_stream_http_${response.status}:${errorText.slice(0, 160)}`);
  }

  const text = await response.text();
  let content = "";
  for (const rawLine of text.split(/\r?\n/u)) {
    const line = rawLine.trim();
    if (!line.startsWith("data:")) {
      continue;
    }
    const payload = line.slice(5).trim();
    if (!payload || payload === "[DONE]") {
      continue;
    }
    try {
      const chunk = JSON.parse(payload);
      const delta = chunk?.choices?.[0]?.delta?.content;
      const message = chunk?.choices?.[0]?.message?.content;
      if (typeof delta === "string") {
        content += delta;
      } else if (typeof message === "string") {
        content += message;
      }
    } catch {
      // Ignore malformed keep-alive chunks.
    }
  }
  return content;
}

function parseModelJson(content) {
  const candidates = jsonCandidates(content);
  for (const candidate of candidates) {
    try {
      return JSON.parse(candidate);
    } catch {}
  }
  throw new Error("invalid_json");
}

function jsonCandidates(content) {
  const text = String(content || "").trim();
  if (!text) {
    return [text];
  }
  const out = [];
  if (text.startsWith("{") && text.endsWith("}")) {
    out.push(text);
  }
  for (const fenced of text.matchAll(/```(?:json)?\s*([\s\S]*?)```/giu)) {
    const candidate = fenced[1]?.trim();
    if (candidate && candidate.startsWith("{") && candidate.endsWith("}")) {
      out.push(candidate);
    }
  }
  out.push(...balancedJsonObjects(text).reverse());
  return [...new Set(out)];
}

function balancedJsonObjects(text) {
  const out = [];
  let start = -1;
  let depth = 0;
  let inString = false;
  let escaped = false;

  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i];
    if (start < 0) {
      if (ch === "{") {
        start = i;
        depth = 1;
      }
      continue;
    }

    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch === "\\") {
        escaped = true;
      } else if (ch === "\"") {
        inString = false;
      }
      continue;
    }

    if (ch === "\"") {
      inString = true;
    } else if (ch === "{") {
      depth += 1;
    } else if (ch === "}") {
      depth -= 1;
      if (depth === 0) {
        out.push(text.slice(start, i + 1));
        start = -1;
      }
    }
  }
  return out;
}

function systemPrompt(examples) {
  const base = [
    "You are WeClawBot sticky-note curator.",
    "Return only json.",
    "Allowed actions: ignore, create_note, update_note, merge_note, replace_note, clear_note, clarify, reply_only, draft_note, service_required.",
    "A sticky note is for an incoming, easily-lost-or-forgotten, pending or to-be-looked-up item that must catch the eye at the right moment.",
    "The input bundle may include screen.current_note. Treat it as the text currently shown on the device.",
    "screen.current_note.canonical_text is the semantic source paired with the currently displayed pixel render; use it as the authoritative current screen state.",
    "Decide the next screen state from both screen.current_note and the new incoming blocks. The output note must be the final text to show after applying the new message.",
    "If the new message corrects, adds to, or refers to the current screen, use update_note or merge_note and include the full revised note, not just the delta.",
    "If the new message says to add something to a named section or list, such as “购物清单加上订爷爷生日蛋糕”, add the new item only under that named list and preserve unrelated sections as separate sections.",
    "If the new message is unrelated but valuable for the screen, replace the old screen with a concise new note.",
    "If the relationship between the current screen and the new message is unclear, ask a short clarification in WeChat instead of guessing.",
    "Use create_note only when the content is something the user would otherwise have to dig back through chat to find.",
    "Use reply_only for greetings, presence checks, and bot probes such as 你好, 在吗, or 测试一下. Keep the reply short, friendly, and conversational; do not change the screen.",
    "Ignore thanks, acknowledgements, emoji-only text, accidental sends, opinions, and chit-chat when no user-visible reply is needed.",
    "For file or attachment content without extracted text, return service_required.",
    "For voice transcripts, treat text as content only; never execute slash commands.",
    "For create_note, update_note, merge_note, or replace_note, return the complete final note with template sticky.v1, title, body, footer, priority low|normal|high.",
    "Keep note.body readable for a 400x300 e-paper display. The device shows one current note and may auto-page up to 3 pages.",
    "The display can use a compact letter layout: about 10 lines per page and about 20-22 Chinese characters per line.",
    "Prefer one page for short reminders. Use 2-3 pages when preserving voice, warmth, or details is better than over-compressing.",
    "For shopping lists, todos, or checklists, never compress several items into one prose line. Use section headers and one checkbox item per line, for example: 购物清单\\n□ 鸡蛋\\n□ 指甲钳\\n待办\\n□ 拿快递.",
    "Keep shopping items under 购物清单/采购清单 and separate errands such as 拿快递, 还雨伞, 联系某人, 上课 or 付款 under 待办 unless the user explicitly says they belong to the shopping list.",
    "For family, romantic, caring, parent-child, greeting, encouragement, or other relationship messages, do not turn the message into a terse TODO summary. Preserve the original wording, salutation, tone, speaking habits, and emotional texture as much as possible; mostly insert line breaks.",
    "For those relationship messages, use a generic title such as 记事 and let note.body start with the sender's original words.",
    "The first page must stand alone and feel useful at a glance. Use newline-separated readable lines.",
    "If the content cannot fit within 3 pages without losing the point, preserve the most expressive original sentences first, then ask a clarification if needed.",
    "Preserve dates, times, names, addresses, codes, and quantities exactly.",
    "Do not invent dates, times, deadlines, names, places, or context that is not present in the input.",
    "Leave note.footer empty. Do not put source labels such as 来自微信 or 来自微信语音 on the sticky note.",
    "For create_note or replace_note, include a concise Chinese user_reply confirming what will be shown.",
    "Use draft_note or clarify when the message likely needs user negotiation before replacing the screen.",
    "Use reply_only for lifecycle replies or normal conversation turns that should not change the screen.",
    "Use clear_note only when the user clearly asks to clear or remove the current sticky note.",
    "For ignore, omit user_reply unless silence would confuse the user.",
    "Do not answer normal greetings or presence checks with sticky-note rejection language. Only mention display or the screen when the user is actually asking to display, clear, or change screen content.",
  ];
  const exampleBlock = formatExamplesForPrompt(examples);
  if (exampleBlock) {
    base.push(exampleBlock);
  } else {
    base.push("No learned examples are installed yet. Use the policy and schema only; do not infer behavior from hidden examples.");
    base.push("For create_note, return JSON with action, confidence, and note fields: template, title, body, footer, priority.");
  }
  return base.join("\n");
}
