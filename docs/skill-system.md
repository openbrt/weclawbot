# Curator Skill System

WeClawBot skills are shareable behavior packages for turning WeChat content
into one current screen note. A Tencent Cloud Function is one runtime adapter
for executing a skill; it is not the skill itself and not the full conversation
lifecycle coordinator.

## Principle

The skill is the product asset. The runtime only supplies compute, secrets,
network access, billing limits, and logs.

One skill should be able to run in:

- a Tencent Cloud Function for the low-cost production path;
- the `weclawbot` host for development and emergency fallback;
- an offline evaluator for regression tests and model distillation.

## Skill Package

```text
skills/package-pickup/
├── skill.yaml
├── policy.md
├── schemas/
│   ├── request.schema.json
│   └── response.schema.json
├── rules/
│   └── index.ts
├── prompts/
│   └── curator.md
├── renderers/
│   └── sticky-v1.json
├── eval/
│   └── cases.jsonl
└── adapters/
    ├── tencent-scf.ts
    ├── node-host.ts
    └── offline-eval.ts
```

`policy.md` is human-readable behavior. `rules/` handles cheap deterministic
decisions. `prompts/` and model routing handle uncertain cases. `renderers/`
describe how a valid note should fit the 400 x 300 RLCD display.

## Note Operations

Skills output constrained note operations, not arbitrary UI code:

| Operation | Meaning |
| --- | --- |
| `ignore` | No device change |
| `reply_only` | Reply in WeChat without changing the screen |
| `draft_note` | Send a proposed note back to WeChat for negotiation |
| `create_note` | Overwrite the current screen note |
| `replace_note` / `update_note` | Replace the current note after feedback |
| `clear_note` | Clear the current note and show the idle pet |
| `clarify` | Ask the sender for missing context in WeChat |
| `service_required` | Explain that external runtime support is required |

The screen is a one-note surface. A long note may paginate automatically, but
new accepted content overwrites the old note rather than creating a local list.
Additional operations must be introduced behind explicit schema versions so
older devices can reject unsupported actions safely.

## Display Contract

A skill may choose a display template and provide structured content, but it
must not draw arbitrary graphics. The cloud function validates output before
sending it to the device.

```json
{
  "action": "create_note",
  "note": {
    "template": "sticky.v1",
    "title": "取件",
    "body": "今天 18:00 前到驿站取 3-2156",
    "footer": "",
    "priority": "normal",
    "expires_at": "2026-06-08T23:59:59+08:00"
  }
}
```

The device renderer decides the final pixels. A skill can influence title,
body, priority, expiration, and page grouping, but cannot ship font files,
scripts, HTML, SVG, or bitmap assets to the device. `footer` defaults to an
empty string and must not be used for source labels such as WeChat, voice, PDF,
or sender names.

## Shareable Skill Types

Good first shared skills:

- pickup-code and delivery reminders;
- family message board;
- meeting action items;
- property-management and utility notices;
- medicine and appointment reminders;
- travel itinerary snippets;
- shopping lists;
- plain sticky-note cleanup and rewriting.
- image, PDF, document, slide, sheet, and voice-to-sticky extraction through
  the Curator Skill Runtime.
- WeChat voice transcript cleanup, using `voice_item.text` as text input
  without extra transcription.

Bad shared skills:

- skills that require unrestricted conversation history;
- skills that need the user's WeChat token;
- skills that send raw private messages to third parties without consent;
- skills that depend on long-running cloud memory for ordinary text curation.

## Marketplace Rules

Shared skills must be versioned, signed, and reviewable. A published skill
contains behavior, examples, schemas, tests, and optional model-routing hints.
It must not contain secrets, provider API keys, device credentials, or mutable
remote code.

Each device pins a skill id and version. Updating a skill is a separate user
choice, not an implicit firmware update.

## Evaluation

Every skill ships with regression cases:

```jsonl
{"text":"你好","expect":{"action":"ignore"}}
{"text":"明天下午三点去物业取门禁卡","expect":{"action":"create_note","contains":"物业取门禁卡"}}
{"text":"文件发你了","expect":{"action":"service_required"}}
```

A candidate skill version cannot be published until it passes:

- schema validation;
- deterministic rule tests;
- privacy checks;
- regression cases;
- a small adversarial set for greetings, duplicates, accidental sends, and
  unsupported files.

## Cloud Function Adapter

The Tencent Cloud Function adapter is responsible for:

- authenticating the device request;
- selecting the pinned skill version;
- running deterministic rules;
- calling the routed model only when needed;
- validating the skill response;
- returning a normalized decision to the ESP32.

This keeps the function tiny and cheap while allowing the valuable behavior to
be shared as skills.

Attachment skills share the same note-operation schema and run through the
normalized content-bundle pipeline described in
[curator-skill-runtime.md](curator-skill-runtime.md) and
[file-processing.md](file-processing.md).
