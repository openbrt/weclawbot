# Cloud Agent Contract

The WeClawBot cloud agent decides whether a WeChat event already received by
the ESP32 should become the current physical screen note. It is a message
curator and conversation lifecycle coordinator, not a transparent relay and not
a WeChat message receiver.

A single stateless cloud function is useful as a skill runtime, but it is not
enough to negotiate screen content with a WeChat user. The cloud side needs
session state for drafts, confirmations, corrections, clears, and the active
screen note. That state may live in a tiny orchestrator service, a serverless
KV/database plus functions, or a user-hosted provider endpoint.

The next product path is MQTT/WSS-first: iLink reaches the ESP32, then the
firmware publishes a bounded event to the gateway or paired Agent. The legacy
HTTP `curator_url` request/response shape remains useful for migration and
local testing, but it is not the long-term user-facing configuration surface.

## Availability Boundary

The ESP32 always owns QR login, the WeChat token, and the `getupdates` loop.
The cloud agent must never receive the WeChat token or become a dependency for
receiving messages.

When the agent is unavailable, the device:

1. Continues calling `getupdates`.
2. Deduplicates and stores text candidates in a bounded local pending queue.
3. Handles device commands locally.
4. Retries pending MQTT/WSS event delivery with bounded backoff.
5. Processes queued candidates when the gateway or Agent returns.

Agent downtime may delay note curation, but it must not lose or block incoming
WeChat messages.

All learned inference belongs to the cloud-agent side of this boundary. The
ESP32 must not run or download a model, even after a smaller specialist model
replaces the initial frontier-model route. The cloud function runs
deterministic rules first and calls a model only for uncertain candidates.

## Decision Actions

Every inbound event produces exactly one lifecycle action:

| Action | Meaning |
| --- | --- |
| `ignore` | Do not update the device or interrupt the user |
| `reply_only` | Reply in WeChat without changing the screen |
| `draft_note` | Propose a screen note in WeChat without displaying it yet |
| `create_note` | Overwrite the screen with one useful current note |
| `replace_note` / `update_note` | Replace the current note after user feedback |
| `clear_note` | Clear the current note and return the device to the pet idle state |
| `clarify` | Ask a question in WeChat before creating a note |
| `service_required` | Explain that file or rich-media processing needs the platform service |

The screen is a one-note surface. New accepted content overwrites the previous
note. Long content may paginate on the device, but it remains one message.

## Default Policy

Use `reply_only` for greetings, presence checks, and bot probes. The official
WeChat entry is a conversation with an agent, not only a sticky-note inbox, so
short messages such as `在吗` should receive a normal friendly reply and must
not update the screen.

Examples that should normally reply without changing the screen:

```text
你好
在吗
测试一下
hello
ping
```

Use `ignore` for acknowledgements, thanks, emoji-only messages, reactions,
accidental sends, and duplicate events when no user-visible reply is needed.

Examples that should normally be ignored:

```text
哈哈
收到
好的
谢谢
👍
```

Use `create_note` only when the message contains information worth seeing
later, such as a reminder, task, address, pickup code, deadline, short list, or
reference detail. The note must not invent missing facts.

Use `clarify` when a message looks potentially useful but cannot become a
self-contained note without more context.

Use `service_required` for documents, images, voice, and other files that need
download, decryption, OCR, transcription, conversion, storage, or model calls.
The open firmware does not download or parse those files. An attachment runtime
may create a separate file job and later return validated note operations.

WeChat voice messages are different when `voice_item.text` is present: that
server-side ASR transcript is treated as a text candidate and can be curated
without extra transcription. Voice transcripts must not execute local slash
commands; they are content only.

## Legacy HTTP Curation Request

The current firmware still contains a legacy HTTP adapter. When that path is
used, the device sends a text candidate without its WeChat token:

```json
{
  "version": 1,
  "event_id": "wx_...",
  "sender_ref": "local-pseudonymous-id",
  "text": "明天下午三点去物业取门禁卡",
  "ai": {
    "provider": "weclawbot",
    "endpoint": "",
    "model": "",
    "token": ""
  },
  "received_at": "2026-06-07T19:30:00+08:00"
}
```

`event_id` is the idempotency key. The agent must return the same decision when
the device retries the same event.

`ai.provider` selects who supplies model capability. `weclawbot` means the
hosted WeClawBot provider uses server-side credentials. This provider URL
selection is legacy; next-version firmware should choose official/custom Agent
mode and use the built-in MQTT/WSS gateway instead of asking users to configure
or trust arbitrary curator URLs.

## Legacy HTTP Curation Response

The agent returns one normalized decision:

```json
{
  "version": 1,
  "event_id": "wx_...",
  "action": "create_note",
  "note": {
    "content": "明天下午 3 点去物业取门禁卡",
    "created_at": "2026-06-07T19:30:00+08:00"
  }
}
```

The device applies the response and marks the pending event complete. Repeated
responses for the same `event_id` must not create duplicate notes. In the
MQTT/WSS path, the same lifecycle result is expressed as an Agent reply intent,
`activity`, or bounded `screen_document`.

See [model-selection.md](model-selection.md) for the initial model route,
capacity policy, and privacy constraints.

See [serverless-first.md](serverless-first.md) for the preferred stateless
deployment and cost boundary.

See [skill-system.md](skill-system.md) for the shareable skill format and
constrained sticky-note display contract.

See [file-processing.md](file-processing.md) for image, voice, PDF, DOCX,
PPTX, XLSX, CSV, and unknown-file behavior.

## Agent Guardrails

- Prefer `ignore` when confidence is low and the content has no clear future value.
- Keep notes concise enough for a 400 x 300 display.
- Preserve dates, times, names, addresses, codes, and quantities exactly.
- Never place agent explanations or classification reasons on the device.
- Reply in WeChat for clarification or service prompts; do not use the display for them.
- Keep raw messages and device commands auditable so users can understand why a note appeared.
- Never request, store, or proxy the device's WeChat token.
