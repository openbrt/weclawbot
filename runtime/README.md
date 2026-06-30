# WeClawBot Curator Runtime

This is the first runnable slice of the automatic sticky-note curator.

It turns normalized WeChat events and extracted document blocks into validated
sticky-note operations. It has no external dependencies so it can run locally,
on the `weclawbot` host, in CI, or inside a small Tencent Cloud Function zip.

## Commands

```sh
npm run check
npm run eval
npm run curate -- "明天下午三点去驿站取件，取件码 3-2156"
```

The output is a constrained decision:

```json
{
  "action": "create_note",
  "note": {
    "template": "sticky.v1",
    "title": "取件",
    "body": "明天下午 3 点去驿站取件，取件码 3-2156",
    "footer": "",
    "priority": "normal"
  }
}
```

## Evolution Loop

The runtime evolves by changing skills and fixtures, not by changing the ESP32
firmware:

1. Capture explicit user feedback or synthetic edge cases.
2. Add JSONL eval cases.
3. Update rules, prompts, extractors, or model routing.
4. Run `npm run eval`.
5. Package the same runtime entrypoint for Tencent SCF.

Real WeChat messages and documents must not become training or shared eval data
without explicit opt-in.

## Optional Model Cascade

The runtime is rules-first. Model calls are only made when explicitly enabled.
The intended cascade is:

```text
rules -> ModelScope student -> DeepSeek teacher
```

Local student test:

```sh
export MODELSCOPE_API_KEY="..."
npm run curate -- --student "丰巢 8-2-3306"
```

Local teacher fallback test:

```sh
export DEEPSEEK_API_KEY="..."
npm run curate -- --student --teacher "丰巢 8-2-3306"
```

Defaults:

- Student: ModelScope `Qwen/Qwen3.5-27B` via `https://api-inference.modelscope.cn/v1`
- Teacher: DeepSeek `deepseek-v4-flash`

Never put API keys in source files, fixtures, or logs.

## Tencent SCF

`src/adapters/tencent-scf.js` exports `main_handler(event)`. The handler expects
the same event JSON used by the local CLI and returns the validated decision as
an HTTP JSON response.

The SCF adapter reads these environment variables:

- `STUDENT_ENABLED`, `MODELSCOPE_API_KEY`, `MODELSCOPE_MODEL`,
  `MODELSCOPE_BASE_URL`, `STUDENT_MIN_CONFIDENCE`
- `TEACHER_ENABLED`, `DEEPSEEK_API_KEY`, `DEEPSEEK_MODEL`,
  `DEEPSEEK_BASE_URL`, `TEACHER_MIN_CONFIDENCE`
- `WEC_INCLUDE_TRACE`

Build the deployable zip:

```sh
npm run package:scf
```

The SCF handler is:

```text
src/adapters/tencent-scf.main_handler
```
