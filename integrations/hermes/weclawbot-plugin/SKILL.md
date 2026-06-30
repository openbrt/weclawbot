---
name: weclawbot
description: Pair and use a WeClawBot e-paper screen from Hermes.
---

# WeClawBot

WeClawBot is a physical 400 x 300 monochrome e-paper screen. The firmware owns
the status bar, footer, refresh behavior, and physical safety limits. A paired
agent may only send live MQTT control while the screen is online; this is never
an offline message queue.

## Pairing

The user selects **自定义智能体** on the screen configuration page. The screen
shows a six-digit AI agent binding code. Run `hermes weclawbot bind <code>` on
the user's Hermes host. No endpoint URL, Wi-Fi password, WeChat token, or model
API key is entered into Hermes.

## Thinking pet

This plugin automatically sends `thinking` immediately before Hermes calls its
model and sends `idle` after the call. The display restores the exact prior
calendar, photo, or note when the work ends, fails, or reaches its TTL.

Use the `weclawbot_activity` tool only for longer non-LLM work that the user
would benefit from seeing. Always send `idle` in a finally path. Never use the
pet as a permanent idle decoration.

The user can disable automatic model-call activity with
`WEC_HERMES_ACTIVITY_AUTO=0`.

## Screen documents

Use `weclawbot_screen_document` when the user asks the Agent to put a card,
status view, timer, or another intentional visual on the paired screen. Supply
a `weclawbot.screen_document.v1` object with:

- `target: "content"` and `kind: "replace"`;
- the current screen `base_revision` (an empty string only for the first page);
- a future UTC expiry;
- one to three same-sized `mono1` pages, each no larger than 368 x 206.

Treat this skill as a hardware contract and starting point, not as a fixed house
style. Preserve any layout preferences, visual language, page rhythm, font
choices, or review habits that the user and Agent have already developed, unless
the user asks to change them or they violate the device bounds above. Skill
upgrades should be additive and must not reset accumulated user-agent practice.

Firmware will not split a single pixel page after receiving it. When possible,
inspect the rendered pages before publishing. The Agent should evaluate the
preview against the user's preferences and its own learned standards, then
regenerate if the bitmap is hard to read, poorly split, or otherwise unsatisfying.
Keep this loop on the Agent side so layout quality can improve with skills,
tools, and model upgrades without requiring firmware changes.

For clear requests such as “清屏”, “清理屏幕”, or “clear screen”, use
`weclawbot_clear_screen` or `hermes weclawbot clear`. Never emulate clearing with
a blank, white, or black `screen_document`; that creates a new note instead of
clearing firmware state.

The firmware owns the status bar and footer. Never claim a document was shown
unless the live MQTT delivery succeeds; never queue a document for an offline
screen. Do not execute downloaded code or LVGL binaries: the only dynamic
surface is the bounded 1-bit bitmap document.
