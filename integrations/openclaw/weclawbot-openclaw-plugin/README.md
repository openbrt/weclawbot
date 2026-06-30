# WeClawBot Agent CLI

This package installs the cross-platform `weclawbotctl` command for pairing
any local Agent with a WeClawBot screen. It works from OpenClaw, Hermes,
Codex, Claude Code, Gemini CLI, OpenCode, or a shell script through the same
local MQTT profile.

## Tell Your Agent

Give your coding agent one sentence:

```text
Install @openbrt/weclawbotctl and connect to my WeClawBot screen with pairing code 123456.
```

Replace `123456` with the six-digit code shown on the screen. The agent should
then install this npm package, bind the screen, run the online doctor check,
and use `weclawbotctl screen` with a pre-rendered pixel document for visible
screen updates. Use `weclawbotctl thinking` and `weclawbotctl idle` for
temporary work-state overlays.

## Agent Instructions

If a user asks you to install, enable, connect, or pair WeClawBot and gives a
six-digit pairing code, do this autonomously:

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl bind 123456 --name "$AGENT_NAME"
weclawbotctl doctor --online
```

Use your product name for `$AGENT_NAME`, such as `codex`, `claude-code`,
`gemini-cli`, `opencode`, `hermes`, or `openclaw`. If global npm installation
is not available, use the one-shot form:

```bash
npm exec --package @openbrt/weclawbotctl -- weclawbotctl bind 123456 --name "$AGENT_NAME"
npm exec --package @openbrt/weclawbotctl -- weclawbotctl doctor --online
```

Do not ask the user for an MQTT URL, username, password, Wi-Fi password,
WeChat credential, port, or webhook. The pairing code is enough. The local MQTT
credential is stored at `~/.config/weclawbot/agent-mqtt.json` with mode `0600`.

When doing visible work, publish a temporary activity:

```bash
task_id="$(uuidgen 2>/dev/null || cat /proc/sys/kernel/random/uuid)"
weclawbotctl thinking --id "$task_id" --ttl 45
# Do the work.
weclawbotctl idle --id "$task_id"
```

`idle` must use the same id as the active `thinking` message. Newer firmware
rejects stale or unrelated `idle` messages and keeps the visible thinking state.
The bundled OpenClaw bridge publishes `thinking` before every curator job and
`idle` after it finishes, so WeChat-origin official-mode work also gets a visible
processing state without relying on the model to remember it.
In OpenClaw itself, the plugin also registers hooks for direct Telegram/UI agent
turns that mention WeClawBot or the physical screen. Those hooks show the
thinking pet while the turn runs and clear it before the final answer. The
installer enables `plugins.entries.weclawbot.hooks.allowConversationAccess`
because OpenClaw blocks conversation hooks for third-party plugins unless the
user explicitly grants that permission.

To put text, status, diagrams, or images on the screen, render them into a
pre-rendered `weclawbot.screen_document.v1` first. The firmware receives pixels;
it does not lay out text, choose fonts, or split pages for agents.

```bash
weclawbotctl preview /path/to/screen-document.json
weclawbotctl screen /path/to/screen-document.json
```

`screen` waits for the device status topic by default. It exits successfully
only after the firmware reports `applied`; a firmware `rejected` status or a
timeout is a failed delivery. Use `--no-wait` only for diagnostics where MQTT
publish acknowledgement is enough.
`preview` renders the exact mono1 pages into PNG files. `screen` also emits a
`preview.pages[].path` list by default. Agents should inspect those images when
possible so the user sees the actual effect, not just "published".
When the OpenClaw tool `weclawbot_publish_screen_document` is used, its return
value contains the same `preview.pages[].path` list. Starting in
`@openbrt/weclawbotctl@0.1.19`, successful `screen` publishes also write a
preview manifest. The OpenClaw hook scans those manifests at the end of the
current run and sends the PNG preview through the active Telegram/UI route when
possible. This catches both plugin-tool calls and indirect CLI calls made from
Python or shell scripts. Set plugin config `auto_preview=false` only if the
site wants to disable this automatic preview delivery and make the Agent handle
media delivery entirely by itself.

The purpose of sending the preview is not just proof that MQTT worked. The
preview is the feedback surface that lets the user's agent learn the user's
reading habits, layout taste, density tolerance, font preferences, and page
rhythm over time. Treat "send to screen" as a two-part delivery: the device is
updated, and the user receives the same effect image in the current chat unless
the agent reports a concrete preview-delivery failure.

To clear the current note, use the firmware clear command:

```bash
weclawbotctl clear
```

Do not emulate clear by publishing a blank, white, or black screen document.
That creates a new note and can leave the physical screen looking black.

The package also includes an OpenClaw integration: the `weclawbot-curator`
skill, `weclawbot_status`, `weclawbot_validate_screen_document`,
`weclawbot_clear_screen`, `weclawbot_publish_screen_document`,
`weclawbot_validate_activity`,
`weclawbot_publish_activity`, and a small outbound bridge service. The bridge
polls `weclawbot.link`; no public HTTP endpoint, port forwarding, or WeChat
credential is required on the OpenClaw host.

OpenClaw plugin tools require OpenClaw `2026.6.9` or newer because the plugin
declares `contracts.tools`. Older OpenClaw builds can still run
`weclawbotctl` as a shell command, but the agent tool injection will not be
reliable.

## Install from npm

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl status
```

For one-shot use without a global install:

```bash
npm exec --package @openbrt/weclawbotctl -- weclawbotctl status
```

`weclawbotctl` is the common pairing and MQTT profile manager for any local
Agent.

To install the OpenClaw plugin itself from npm after the package is published:

```bash
weclawbotctl openclaw install
weclawbotctl openclaw doctor
```

This wraps:

```bash
openclaw plugins install @openbrt/weclawbotctl --pin --force
openclaw plugins enable weclawbot
```

Restart the OpenClaw gateway or app after installation so it reloads plugin
tools. The doctor checks the OpenClaw version, plugin installation, plugin
diagnostics, hook permission, and local gateway reachability. If a local WSS gateway uses a
self-signed certificate, the doctor will suggest `NODE_EXTRA_CA_CERTS`. If the
certificate lacks `localhost` or `127.0.0.1` SANs, fix the gateway certificate
or use a certificate trusted by Node. The package does not rewrite other
users' OpenClaw gateway certificates automatically.

If OpenClaw is installed outside `PATH`, pass it explicitly:

```bash
weclawbotctl openclaw doctor --bin /path/to/openclaw
```

When `weclawbotctl` is installed globally, it also checks the same npm global
`bin` directory for `openclaw`, which helps non-interactive SSH and systemd
environments where shell startup files are not loaded.

To install the OpenClaw plugin from a local checkout during development:

```bash
openclaw plugins install /path/to/weclawbot-openclaw-plugin
```

For a dedicated low-latency curator agent, create it and give it only this
plugin's skill and workspace instructions:

```bash
openclaw agents add weclawbot --workspace ~/.openclaw/workspace-weclawbot
# Find the agent index with: openclaw config get agents.list --json
openclaw config set 'agents.list[<weclawbot-index>].skills' '["weclawbot-curator"]' --strict-json
cp /path/to/weclawbot-openclaw-plugin/workspace/AGENTS.md ~/.openclaw/workspace-weclawbot/AGENTS.md
```

Create `~/.config/weclawbot/openclaw-curator.env` with mode `0600`:

```ini
WEC_GATEWAY_URL=https://weclawbot.link
WEC_GATEWAY_TOKEN=paired-worker-token
WEC_OPENCLAW_AGENT=weclawbot
WEC_OPENCLAW_TRANSPORT=gateway
WEC_OPENCLAW_THINKING=off
```

Install and start the user service:

```bash
mkdir -p ~/.config/systemd/user
cp /path/to/weclawbot-openclaw-plugin/systemd/weclawbot-openclaw-curator.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now weclawbot-openclaw-curator
```

## Security boundary

- The ESP32 keeps its WeChat login credential and `getupdates` loop.
- The bridge receives only normalized message events, screen context, and media
  metadata needed for curation.
- Put `DEEPSEEK_API_KEY` in OpenClaw's private configuration, never in this
  plugin, the firmware, a device setting, or a public repository.
- The agent may return only a WeClawBot decision. It cannot alter firmware,
  Wi-Fi, or the device's WeChat credential.
- The bridge adapts common model field aliases to the stable WeClawBot note
  contract before the gateway renders a monochrome preview.
- `WEC_OPENCLAW_TRANSPORT=gateway` reuses the host's OpenClaw Gateway. Set it
  to `local` only for an intentionally gateway-free installation.

## Agent direct control

The normal bridge is message-triggered by WeChat. Scheduled cards and other
agent-originated updates use a separately paired MQTT/TLS control channel;
they are never placed in a gateway mailbox. A paired Agent can publish a
pre-rendered screen document immediately:

```bash
weclawbotctl doctor --online
weclawbotctl preview /path/to/screen-document.json
weclawbotctl screen /path/to/screen-document.json
```

The plugin exposes both validators and publish tools. The validator reports
`direct_delivery_ready:false` when a supplied event contract says that specific
inbound event cannot use the live document path, but that does not disable a
locally paired `weclawbotctl` profile. Before saying direct screen delivery is
unavailable, run `weclawbotctl status` or `weclawbotctl doctor --online`.

The pairing UX deliberately requires no user-supplied Agent endpoint: choose
**自定义智能体** in the device configurator, then enter the six-digit code shown
on screen:

```bash
weclawbotctl bind 123456 --name openclaw
weclawbotctl status
weclawbotctl doctor --online
```

Or without a global install:

```bash
npm exec --package @openbrt/weclawbotctl -- weclawbotctl bind 123456 --name openclaw
npm exec --package @openbrt/weclawbotctl -- weclawbotctl doctor --online
```

`weclawbot-byoa-bind 123456` remains as a compatibility alias. The command
stores a credential scoped to that one screen with mode `0600`. No WeChat scan
is required for direct Agent control.

## MQTT profile

The saved credential follows the same operator pattern as common MQTT clients:
keep a local connection profile, use TLS/WSS, keep the client id stable and
non-secret, and avoid putting passwords in shell history. Export the profile
only when another local coding agent or MQTT tool needs it:

```bash
weclawbotctl export --format env
weclawbotctl export --format json --output ~/.config/weclawbot/agent-mqtt.masked.json
weclawbotctl export --format mosquitto --include-secret --output ~/.config/weclawbot/mosquitto.conf
```

Exports mask the password unless `--include-secret` is supplied. Remove the
local credential with `weclawbotctl unbind --yes`.

Because `weclawbotctl` is a plain command, Codex, Claude Code, Gemini CLI,
OpenCode, Hermes, OpenClaw, or a shell script can all reuse the same paired
MQTT profile. While an agent works, it can publish a short-lived thinking
activity so the screen becomes a useful live side display:

```bash
task_id="$(uuidgen)"
weclawbotctl thinking --id "$task_id" --ttl 45
# Run an LLM call, retrieval, or other task.
weclawbotctl idle --id "$task_id"
```

To put content on screen, pass a pre-rendered monochrome document to the same
credential. The document must use the live revision in the last
`device_context` (an empty revision is valid for the first document), one to
three `mono1` pages, and a future UTC expiry. Agents may use PIL, Canvas, SVG
rasterization, screenshots, or any local renderer, but the MQTT payload must be
pixels:

There is no canonical WeClawBot renderer that agents must use. The stable
contract is the bounded pixel document plus device feedback. Keep layout,
typography, and page-composition decisions in the agent/tool layer so skills and
models can improve the result without requiring users to flash firmware.
Preserve any layout preferences, visual language, page rhythm, font choices, or
review habits that the user and agent have already developed; package upgrades
should add capabilities without resetting that accumulated practice.

The hardware facts are stable: the content viewport is 368 x 206 mono1 pixels,
content documents may contain one to three pages, and the firmware will not split
a single pixel page after receiving it. If `pages.length === 1`, the physical
screen has exactly one page.

Before publishing, agents should inspect or otherwise self-evaluate the rendered
pages against the user's preferences and their own learned standards when their
runtime supports it. Regenerate the document if the bitmap does not satisfy those
standards. After a successful publish, the user should receive the preview PNGs
through the current chat/media channel. In OpenClaw this is handled by the
manifest hook when possible; in other Agents the Agent should send the files
itself. The user can then see the effect and correct the agent's choices. Carry
those corrections forward instead of resetting local style memory on plugin
upgrades.

```bash
weclawbotctl preview /path/to/screen-document.json
weclawbotctl screen /path/to/screen-document.json
```

When the user explicitly asks to overwrite whatever is currently shown, and
the firmware supports forced replacement, use:

```bash
weclawbotctl screen /path/to/screen-document.json --force
```

The command waits for the device's `applied`/`rejected` status by default, so
an Agent must not tell the user the content is on the screen until the command
returns success. If the device reports `stale_screen_revision`, regenerate the
document with the current revision or intentionally overwrite with `--force`.

These commands use MQTT/TLS directly, publish QoS 1 without retain, and
never create an offline command queue. See
`docs/agent-direct-control-protocol.md` in the firmware repository for the
wire contract and security model.
