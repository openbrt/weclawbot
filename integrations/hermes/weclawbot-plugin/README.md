# WeClawBot for Hermes

This Hermes plugin turns a paired WeClawBot screen into a small, physical work
indicator. During real Hermes model calls the e-paper screen shows its thinking
pet, then restores the prior screen when the call completes.

## Install

```bash
git clone https://github.com/openbrt/weclawbot.git
cp -R weclawbot/integrations/hermes/weclawbot-plugin ~/.hermes/plugins/weclawbot
python -m pip install -r ~/.hermes/plugins/weclawbot/requirements.txt
```

Enable `weclawbot` in Hermes' `plugins.enabled`, then restart Hermes.

## Pair a screen

1. On the WeClawBot configuration page, choose **自定义智能体** and restart it.
2. Read the six-digit `AI智能体绑定码` shown by the screen. The code expires
   quickly; refresh the screen if it has timed out.
3. On the Hermes host, run:

```bash
hermes weclawbot bind 123456
hermes weclawbot status
hermes weclawbot doctor --online
```

The plugin stores only a device-scoped MQTT credential at
`~/.config/weclawbot/agent-mqtt.json` with mode `0600`. It never stores the
screen's Wi-Fi password, WeChat login, or model credential.

## MQTT profile

The local credential is a saved MQTT profile, similar to desktop MQTT clients:
server URL, client id, username, password, and the scoped control topic. Do not
copy the password into prompts or command history. To hand the same profile to
another local coding agent or MQTT tool, export it explicitly:

```bash
hermes weclawbot export --format env
hermes weclawbot export --format json --output ~/.config/weclawbot/agent-mqtt.masked.json
hermes weclawbot export --format mosquitto --include-secret --output ~/.config/weclawbot/mosquitto.conf
```

Exports mask the password unless `--include-secret` is supplied. Remove the
local credential with:

```bash
hermes weclawbot unbind --yes
```

## Manual activity

Hermes automatically signals model calls. A tool or long local workflow can
also use `weclawbot_activity` with `state: thinking`, a `ttl_seconds` value of
5-120, and then `state: idle` using the same `correlation_id`.
Newer firmware rejects stale or unrelated idle ids and keeps the active thinking
state.

The device accepts live QoS 1 MQTT messages only while it is online. It has no
retained control topic or offline command inbox.

The same activity state is available from the command line, which lets Codex,
Claude Code, Gemini CLI, OpenCode, or a shell script drive the screen without a
Hermes-specific API:

```bash
task_id="$(uuidgen)"
hermes weclawbot thinking --id "$task_id" --ttl 45
hermes weclawbot idle --id "$task_id"
```

## Put a document on screen

The plugin exposes `weclawbot_screen_document`, or the equivalent command:

```bash
hermes weclawbot screen /path/to/screen-document.json
```

The Agent is free to choose the content and layout, but the document must be
pre-rendered as one to three `mono1` pages inside the 368 x 206 content area.
Firmware will not split a single pixel page after receiving it; `pages.length`
is the page count on the physical screen. Preserve user-agent layout preferences
across plugin upgrades unless they violate the hardware limits or the user asks
to change them.
It must reference the live `base_revision` from the screen context and carry a
future UTC expiry. The plugin validates these hardware limits before sending.
