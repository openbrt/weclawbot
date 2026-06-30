# Curator Model Selection

Decision date: 2026-06-07

## Deployment Boundary

Models are server-side components only. They may run on the persistent
`weclawbot` host or in Tencent Cloud Functions, but never on the ESP32.

The purpose of training a smaller specialist model is to reduce server cost,
latency, and dependence on free frontier-model capacity while increasing
concurrency. It is not an attempt to move inference onto the display device.

## Selected Route

| Stage | Model | Role |
| --- | --- | --- |
| Private development | Rules-first runtime | Deterministic local evaluation with no accidental model spend |
| Student | ModelScope `Qwen/Qwen3.5-27B` | Low-cost first model for uncertain short text |
| Teacher | `deepseek-v4-flash` | High-confidence fallback when the student is unsure |
| Failure fallback | Local pending queue | Delay curation; never block `getupdates` |

The production route is a confidence cascade:

```text
rules -> ModelScope student -> DeepSeek teacher
```

ModelScope is used as the first student provider because its API-Inference
endpoint is OpenAI-compatible and the account exploration found callable free
Qwen models. The default student is `Qwen/Qwen3.5-27B` for text curation.
Attachment understanding can later use the verified VL models through a
separate file-processing skill; it should not be mixed into the short-text
sticky-note path.

Operational notes from the first deployment:

- `Qwen/Qwen3.5-35B-A3B` can produce good text decisions but has unstable latency
  for this hot path.
- `Qwen/Qwen3.5-27B` returned within a few seconds in local and SCF smoke tests.
- ModelScope can occasionally return an empty non-streaming response, so the
  runtime retries once with streaming before falling back.
- The student cannot veto a rule-created sticky note. If it tries to downgrade a
  `create_note` to `ignore`, `clarify`, or `service_required`, the runtime keeps
  the rule note and lowers confidence so the teacher can adjudicate when enabled.

`deepseek-v4-flash` remains the teacher because the task is Chinese short-message
classification and concise extraction, the official endpoint is
OpenAI-compatible, and JSON output lets the runtime validate the decision schema.
It should return only the `ignore`, `create_note`, `clarify`, or
`service_required` decision schema.

Use OpenCode Zen free models only as offline challengers for synthetic cases and
explicitly authorized evaluation. ModelScope student capacity is still treated
as replaceable infrastructure, not as a product guarantee.

Do not use `nemotron-*free` endpoints for real WeChat content. They are
excessive for this small classification task, and OpenCode documents the
NVIDIA free endpoints as trial-only with logged prompts and outputs.

## Capacity Policy

ModelScope and DeepSeek both expose OpenAI-compatible chat-completions endpoints.
Model calls are optional in local development and must be enabled through
environment configuration.

- Start with two concurrent curator workers.
- Use a short request timeout and bounded retries.
- On `429`, timeout, or provider failure, keep the event pending on the device.
- Never fall back to rendering raw chat.
- Never let agent availability affect the device `getupdates` loop.

## Privacy Policy

Free Zen models may use submitted data to improve models. Before sending real
messages, the product must clearly disclose this behavior and obtain user
consent.

The device sends only the minimum text candidate and a pseudonymous sender
reference. It never sends its WeChat token, QR credential, message cursor, or
unrelated conversation history.

Official references:

- [DeepSeek API quick start](https://api-docs.deepseek.com/)
- [DeepSeek chat completions](https://api-docs.deepseek.com/api/create-chat-completion)
- [DeepSeek JSON output](https://api-docs.deepseek.com/guides/json_mode/)

See [cloud-agent-choice.md](cloud-agent-choice.md) for the selected runtime,
agent framework, model gateway, and non-choices.
