# WeClawBot

WeClawBot is an open ESP32-S3 reflective-LCD desk screen for the Waveshare
ESP32-S3-RLCD-4.2 board. It turns a small 400 x 300 black-and-white display
into a persistent place for notes, reminders, photos, status cards, and
agent-generated dashboards.

The same firmware supports two connection shapes:

- **WeClawBot official mode**: ordinary users scan the WeChat QR code on the
  screen. The official cloud Agent curates the message and pushes a bounded
  screen update back to the device.
- **BYOA / custom Agent mode**: advanced users select **自定义智能体** in the
  configuration page. The screen shows a six-digit pairing code. OpenClaw,
  Hermes, Codex, Claude Code, Gemini CLI, OpenCode, or any local script can
  install `@openbrt/weclawbotctl`, bind with that code, and control the screen
  over MQTT/WSS. The user does not type a gateway URL, expose a port, or share
  a WeChat credential.

The firmware remains deliberately simple: it owns Wi-Fi, display refresh,
buttons, local state, and hardware boundaries. Agents own intent, layout,
fonts, page splitting, preview review, and user-facing conversation. This keeps
the hard-to-update firmware stable while agent skills and user preferences can
evolve quickly.

## Hardware

- Board: Waveshare ESP32-S3-RLCD-4.2
- Display: 400 x 300 landscape RLCD
- Buttons:
  - `KEY`: GPIO18, active low
  - `BOOT`: GPIO0, active low

## Features

- Official WeChat QR login on the screen
- BYOA pairing-code flow for user-owned Agents
- MQTT/WSS direct-control path for `screen_document`, `activity`, and
  `screen_clear`
- Text messages become curated local sticky notes in official mode
- Cloud/Agent message curation contract: greetings, acknowledgements, emoji,
  duplicates, and other low-value chat do not reach the display
- Local note persistence in NVS
- Image, file, and voice workflows are routed through the Agent/runtime layer
  when configured; unsupported paths show a clear service notice
- RedBlock desk-pet idle/startup screen and cloud-processing "thinking" state,
  adapted for the black-and-white RLCD
- Short-lived BYOA/Agent thinking overlay that returns to the previous page
- One to five visible screens depending on state: calendar, idle photo, and up
  to three message pages
- Button-based page flipping and clear actions
- Browser install/configuration page through Web Serial
- Planned self-contained USB MSC product disk with offline help, install page,
  and configuration entry bundled on the device

## Button Map

| Button | Action | Behavior |
| --- | --- | --- |
| Left / KEY short | Previous page | Flip to the previous visible screen or message page |
| Left / KEY long, 3s | Clear text | Clear text notes, keeping the idle photo and WeChat login |
| Right / BOOT short | Next page | Flip to the next visible screen or message page |
| Right / BOOT long, 5s | Full clear | Clear photo, text, WeChat login, and local pairing state, then return to the selected mode's setup screen |

The board only has two practical user buttons, so destructive operations are
kept on long press.

## Build

Install ESP-IDF 5.4 or newer, then:

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py flash monitor
```

This workspace also includes a helper that wraps the active ESP-IDF `idf.py`
environment. Source ESP-IDF first, or pass `IDF_PATH` / `PYTHON_BIN`
explicitly if you use a local venv:

```sh
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
./scripts/idf.sh menuconfig
./scripts/idf.sh flash monitor
```

Build channels:

- Development builds may enable `CONFIG_WEC_DEVELOPMENT_BUILD=y` and show
  `微笺（开发版）` in the device header. These are for local/private iteration
  and should not be treated as the public release number.
- Public release builds must keep `CONFIG_WEC_DEVELOPMENT_BUILD` disabled and
  show `微笺屏`. Use `sdkconfig.release.defaults` when preparing public release
  artifacts and the browser installer bundle.
- The local configuration page does not promise a hard-coded public firmware
  number in its header. After a device is connected, the page reads the runtime
  `WEC:` status and shows that device's actual firmware version.

For local development you can still set build-time defaults in `menuconfig`:

- `WeClawBot -> Wi-Fi SSID`
- `WeClawBot -> Wi-Fi password`

You can also keep a private `sdkconfig.defaults.local` and merge it in your own
build workflow. Do not commit Wi-Fi credentials.

At runtime the firmware also accepts line-based serial configuration on USB
Serial/JTAG or UART0:

```text
WEC:HELLO
WEC:GET
WEC:SET {"ssid":"your-wifi","password":"your-password"}
WEC:SET {"agent_mode":"official"}
WEC:SET {"agent_mode":"byoa"}
WEC:REBOOT
WEC:CLEAR_WIFI
WEC:CLEAR_WECHAT
WEC:CLEAR_AGENT
WEC:CLEAR_NOTES
```

Replies are emitted as `WEC:{...}` JSON lines so host tools can ignore normal
ESP-IDF log output. If no Wi-Fi is configured, the device stays on the RedBlock
USB configuration screen instead of exiting the app.

`agent_mode` is the user-facing switch. `official` starts the WeChat QR /
official Agent path. `byoa` starts the pairing-code flow for a user-owned
Agent. The legacy `curator_url` is kept as an internal/debug transition field;
normal users should not enter service URLs or open firewall ports.

## Local Web Installer

The current installer is served from the development machine or website. The
product direction is to also expose a small USB MSC disk from the device itself,
so first-time users can open help, install, and configuration pages directly
from the mounted `WECLAWBOT` drive. See `docs/usb-msc-product-disk.md`.

After a successful build, prepare the browser-installable firmware parts:

```sh
./scripts/prepare_web_firmware.sh
```

Then serve the local web console:

```sh
python3 scripts/serve_web.py
```

Open:

```text
http://localhost:8765/
```

The left side uses ESP Web Tools to flash bootloader, partition table, OTA data,
and app firmware through Web Serial. It does not write the NVS area at `0x9000`,
so normal firmware updates keep Wi-Fi, Agent mode, local notes, and WeChat
login state. Configuration is lost only if you explicitly erase flash, change
the partition table incompatibly, or use a full merged image that covers NVS.

The right side opens the running firmware's `WEC:` serial protocol to write
Wi-Fi credentials, choose official or BYOA mode, reboot, and clear local device
state. Use desktop Chrome or Edge. For first-time flashing on
ESP32-S3-RLCD-4.2, hold `BOOT` while plugging Type-C, release it after the
serial port appears, and reboot without holding `BOOT` after flashing.

Configuration page behavior:

- **WeClawBot 官方**: save Wi-Fi and mode, reboot, then scan the WeChat QR code
  on the screen. The official Agent handles curation and pushes the result back
  through the live MQTT channel.
- **自定义智能体**: save Wi-Fi and mode, reboot, then read the six-digit binding
  code shown in the calendar area. Tell your Agent:

  ```text
  Install @openbrt/weclawbotctl and connect to my WeClawBot screen with pairing code 123456.
  ```

  Replace `123456` with the code on the screen. The Agent should install the
  npm package, bind the screen, run `weclawbotctl doctor --online`, and use the
  MQTT profile stored in `~/.config/weclawbot/agent-mqtt.json`.

Current build check:

```text
weclawbot.bin: about 5.2 MiB
factory partition: 8 MB
```

## Message Behavior

Official mode:

- Curated text: saved as a sticky note and rendered immediately.
- Greetings, acknowledgements, emoji-only messages, duplicates, and low-value
  chat: ignored by the cloud agent.
- Ambiguous useful messages: clarified in WeChat before becoming a note.
- Voice messages with WeChat-provided transcription: curated as text content;
  voice transcripts cannot execute slash commands.
- `/next`: next note.
- `/prev`: previous note.
- `/help`, `帮助`, or `官网`: reply with the product entry URL and local USB
  disk recovery path.
- `/clear`: clear current note.
- `/clear all`: clear all local notes.
- `修改为...` / `改成...`: replace the current note with the user's corrected
  wording. These feedback edits are the seed data for later curator evolution.
- Image/file/raw voice/PDF/DOCX/PPTX/XLSX/CSV: show and reply with the
  platform-service notice unless an attachment runtime is configured.
  Attachment processing is asynchronous and returns validated sticky-note
  operations later.

BYOA mode:

- WeChat/iLink input is disabled and ignored. The user's Agent owns the screen.
- The Agent publishes `screen_document` pixels, not raw text. Firmware validates
  geometry and applies or rejects the document.
- The Agent may publish a short-lived `thinking` activity before long work and
  must return to `idle` when it finishes.
- Clearing uses `screen_clear`; do not simulate clear by drawing a blank or
  black page.
- The transport is live-only. There is no retained MQTT command and no offline
  mailbox.

The device always owns WeChat QR login, token storage, `getupdates`, Wi-Fi,
display refresh, note/photo storage, and button behavior. Agents and cloud
runtimes own curation, layout, file processing, model calls, and user-visible
workflow. See:

- `docs/architecture.md` for the official/BYOA split
- `docs/agent-direct-control-protocol.md` for MQTT pairing and screen documents
- `docs/byoa-reliability-analysis.md` for chain reliability and open questions
- `docs/cloud-agent.md`, `docs/serverless-first.md`, and
  `docs/cloud-agent-choice.md` for the official Agent path
- `docs/file-processing.md` and `docs/curator-skill-runtime.md` for attachment
  handling and skill execution
- `docs/agent-preview-delivery-discussion.md` for the latest preview-delivery
  work
- `docs/current-progress.md` for the current handoff snapshot

Evolution is driven by user interaction. When WeClawBot ignores, asks for
clarification, creates a note, or receives a correction such as `修改为...`, that
decision and feedback can become a reviewed, privacy-filtered eval case for the
next skill version. Models may help propose changes, but published behavior is
promoted through tests, schemas, and signed skill releases.

## Agent CLI And Plugins

The BYOA path is distributed as an npm package:

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl bind 123456 --name openclaw
weclawbotctl doctor --online
```

For one-shot use without a global install:

```bash
npm exec --package @openbrt/weclawbotctl -- weclawbotctl bind 123456 --name codex
npm exec --package @openbrt/weclawbotctl -- weclawbotctl doctor --online
```

OpenClaw users can install the plugin and hooks in one command:

```bash
weclawbotctl openclaw install
weclawbotctl openclaw doctor
```

The OpenClaw plugin requires OpenClaw `2026.6.9` or newer for tool contracts.
It includes status, clear, activity, screen-document validation, publish tools,
and a curator bridge. In `@openbrt/weclawbotctl@0.1.19`, successful
`screen` publishes write a preview manifest. The OpenClaw hook scans the
manifest at the end of the current run and sends the exact PNG preview back to
the active Telegram/UI route when possible. This makes "上屏" visible to the
user even when an Agent indirectly called `weclawbotctl screen` from a Python or
shell script.

Other Agents such as Hermes, Codex, Claude Code, Gemini CLI, and OpenCode can
use the same CLI profile. The stable contract is:

```bash
weclawbotctl thinking --id "$task_id" --ttl 45
weclawbotctl preview /path/to/screen-document.json
weclawbotctl screen /path/to/screen-document.json
weclawbotctl idle --id "$task_id"
```

`screen` waits for firmware `applied` by default. Preview images are the
feedback surface for layout taste, density, font choice, and page rhythm; they
are not just proof that MQTT published.

## Curator Runtime

The cloud-side sticky-note curator starts in `runtime/`:

```sh
cd runtime
npm run check
npm run eval
npm run curate -- "明天下午三点去驿站取件，取件码 3-2156"
npm run package:scf
```

The runtime is rules-first. Optional DeepSeek calls are enabled only through
environment variables such as `DEEPSEEK_API_KEY`; secrets must never be stored
in this repository.

The stable production entrypoint used internally by devices and agents is:

```text
https://weclawbot.link/gateway
```

`weclawbot.link/gateway` is the official-mode device/Agent entrypoint, not a
field that end users type into the configuration page. It runs the gateway
routing layer while agents are migrated to the agent-owned display protocol. It
keeps structured agent logs on the server at
`/var/log/weclawbot/curator-agent.log` and suppresses low-value `ignore`
replies so greetings and probe messages do not create confusing WeChat chatter.

## History Archive

The previous research workspace has been moved to:

```text
history/esp32-weclawbot-research/
```

That directory is intentionally ignored by Git. It contains useful old
experiments and traces, but it should not be published without a secrets and
artifact audit.
