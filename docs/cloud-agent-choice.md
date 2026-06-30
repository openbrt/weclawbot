# Cloud Agent Choice

Decision date: 2026-06-08

## Selected Stack

| Layer | Choice | Reason |
| --- | --- | --- |
| Runtime | Tencent Cloud Function HTTP-triggered Node.js | On-demand, low idle cost, close to normal web-service development |
| Agent framework | WeClawBot Curator Skill Runtime | The product needs constrained note decisions, not a general tool-using agent |
| Model gateway | DeepSeek official API | OpenAI-compatible endpoint, direct billing, fewer gateway dependencies |
| Development model | Rules-first, optional `deepseek-v4-flash` behind `DEEPSEEK_API_KEY` | Avoid accidental spend and keep local eval deterministic |
| Production model | `deepseek-v4-flash` | Low-latency Chinese curator path with JSON output |
| Fallback | Device pending queue | Delay curation; never render raw chat |

The cloud agent is therefore not Dify, Coze, LangChain, OpenCode CLI, or a
hosted long-running bot. It is a small WeClawBot-specific skill runner deployed
as a cloud function.

## Why This Choice

WeClawBot's core task is narrow:

1. classify a received WeChat event;
2. ignore low-value chat;
3. create or update a sticky note through a constrained schema;
4. ask for clarification or service payment when needed.

That does not require a general agent loop, long-term tool memory, browser
automation, vector search, or a multi-agent framework. A custom skill runner is
smaller, easier to audit, cheaper to run, and safer for user messages.

## Model Route

Use deterministic rules first. Call a model only when the rule layer cannot
decide safely.

```text
text candidate
  -> schema validation
  -> deterministic ignore/service rules
  -> skill prompt
  -> DeepSeek API deepseek-v4-flash
  -> response schema validation
  -> device decision
```

During private development, keep model calls opt-in with `DEEPSEEK_API_KEY`.
The default local path is deterministic rules plus JSONL evaluation. For real
user messages, use the official DeepSeek API directly instead of free gateway
capacity.

OpenCode Zen free models can remain offline challengers for synthetic or
explicitly authorized evaluation, but they are not the first production path.
Do not use NVIDIA `nemotron-*free` endpoints for real or sensitive WeChat
content.

## Non-Choices

| Option | Reason not selected now |
| --- | --- |
| Dify / Coze | Too much product surface and state for a tiny per-message decision loop |
| LangChain / LangGraph | Useful for complex tool chains, unnecessary for this fixed schema path |
| OpenCode CLI agent | Designed for coding workflows; WeClawBot should use DeepSeek as a model API only |
| Persistent host agent | Adds idle cost and failure surface; cloud function is enough for normal text |
| Local LLM on `weclawbot` | The host has 2 CPU cores, 3.6 GiB RAM, and no useful inference GPU |

## Switching Rule

Keep the model behind `MODEL_PROVIDER`, `MODEL_ID`, and `MODEL_ENDPOINT`
configuration. A skill version must pass the same regression set before it can
switch models.

Production may switch away from DeepSeek if privacy, latency, model
availability, cost, or China-network reachability becomes a real constraint. The
skill contract should not change when that happens.

## References

- [Tencent Cloud SCF function overview](https://www.tencentcloud.com/document/product/583/19805)
- [Tencent Cloud HTTP-triggered functions](https://intl.cloud.tencent.com/document/product/583/40688)
- [DeepSeek API quick start](https://api-docs.deepseek.com/)
- [DeepSeek chat completions](https://api-docs.deepseek.com/api/create-chat-completion)
- [DeepSeek JSON output](https://api-docs.deepseek.com/guides/json_mode/)
