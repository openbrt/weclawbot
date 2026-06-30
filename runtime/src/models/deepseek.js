import { curateWithChatModel } from "./chat-curator.js";

// Thin DeepSeek wrapper kept for back-compat. DeepSeek is the default "teacher"
// tier; the generic engine in chat-curator.js does the real work and injects the
// Skill Library few-shot examples.
export async function curateWithDeepSeek(bundle, options = {}) {
  return curateWithChatModel(bundle, {
    role: options.role || "teacher",
    apiKey: options.apiKey || process.env.DEEPSEEK_API_KEY,
    baseUrl: options.baseUrl || process.env.DEEPSEEK_BASE_URL || "https://api.deepseek.com",
    modelName: options.modelName || options.model || process.env.DEEPSEEK_MODEL || "deepseek-v4-flash",
    maxTokens: options.maxTokens,
    timeoutMs: options.timeoutMs,
    skillId: options.skillId,
    skillLibrary: options.skillLibrary,
    examples: options.examples,
  });
}
