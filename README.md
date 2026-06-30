# WeClawBot

WeClawBot is an open firmware and Agent bridge for a small always-on desk
screen built on the Waveshare ESP32-S3-RLCD-4.2 board. The device is a
400 x 300 black-and-white reflective display for persistent notes, photos,
reminders, status cards, and Agent-generated dashboards.

The product has one user promise:

> Put the useful thing on the little screen, quietly and reliably.

There are two ways to own that screen.

- **WeClawBot Official**: ordinary users flash the firmware, configure Wi-Fi,
  scan the WeChat QR code on the screen, and let the official WeClawBot Agent
  understand the message, render it, and push it back over MQTT/WSS.
- **BYOA / Custom Agent**: advanced users choose **自定义智能体** in the
  configuration page. The screen shows a six-digit pairing code. OpenClaw,
  Hermes, Codex, Claude Code, Gemini CLI, OpenCode, or any script can install
  `@openbrt/weclawbotctl`, pair with that code, and control the screen directly.

Users do not type gateway URLs, open firewall ports, or hand WeChat credentials
to an Agent. The ESP32 owns Wi-Fi, WeChat login, local state, buttons, display
refresh, and hardware limits. Agents own intent, layout, preview, file
processing, model calls, and user-facing conversation.

## Install And Configure

Open the public Web Serial console in desktop Chrome or Edge:

```text
https://openbrt.github.io/weclawbot/web/
```

That single page contains both:

- firmware installation through ESP Web Tools;
- device configuration through the firmware `WEC:` serial protocol.

For a first flash, hold `BOOT`, plug in USB-C, wait for the serial port, then
release `BOOT`. Normal upgrades do not erase the NVS area, so Wi-Fi, WeChat
login, Agent mode, notes, and photos survive unless the user explicitly clears
or erases the device.

After flashing:

1. Connect the device in the configuration panel.
2. Save Wi-Fi.
3. Choose **WeClawBot 官方** or **自定义智能体**.
4. Save and reboot.

Official mode shows a WeChat QR code. BYOA mode shows a pairing code.

## Pair A Custom Agent

When the screen shows a six-digit BYOA code, tell your Agent:

```text
Install @openbrt/weclawbotctl and connect to my WeClawBot screen with pairing code 123456.
```

The Agent should install the npm package, bind the screen, run an online doctor
check, and use the MQTT profile stored at
`~/.config/weclawbot/agent-mqtt.json`.

Manual commands:

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl bind 123456 --name openclaw
weclawbotctl doctor --online
```

OpenClaw users can install hooks and workspace guidance with:

```bash
weclawbotctl openclaw install
weclawbotctl openclaw doctor
```

The stable BYOA commands are:

```bash
weclawbotctl thinking --id "$task_id" --ttl 45
weclawbotctl preview /path/to/screen-document.json
weclawbotctl screen /path/to/screen-document.json
weclawbotctl idle --id "$task_id"
weclawbotctl clear
```

`screen` publishes pixels, waits for firmware `applied`, and records a preview
manifest when possible. Preview images are part of the product: they let the
user and Agent tune density, typography, page splitting, and taste over time.

## Hardware

- Board: Waveshare ESP32-S3-RLCD-4.2
- Display: 400 x 300 landscape RLCD, monochrome
- Flash: 16 MB
- Sensor: SHTC3 temperature/humidity
- Buttons:
  - Left / KEY short: previous screen
  - Left / KEY long: clear text notes
  - Right / BOOT short: next screen
  - Right / BOOT long: full clear

The visible screen set is intentionally small: calendar, photo, and up to three
message pages. Left and right buttons flip quickly through everything.

## Firmware Roles

Firmware should stay boring and dependable:

- keep Wi-Fi credentials and WeChat/iLink tokens in local NVS;
- poll WeChat `getupdates` in official mode;
- ignore WeChat input in BYOA mode;
- connect to MQTT/WSS for official and BYOA control;
- validate `activity`, `screen_document`, and `screen_clear`;
- draw already-rendered pixels within the 400 x 300 hardware boundary;
- preserve local state across normal firmware upgrades.

Agents should evolve quickly:

- decide what the user meant;
- choose whether to ignore, clarify, render, or clear;
- produce readable black-and-white layouts;
- split pages when useful, but avoid unnecessary paging;
- send preview images back to the user-facing channel;
- improve from corrections and user taste without requiring firmware upgrades.

## Build

Install ESP-IDF 5.4 or newer.

Development build:

```bash
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
./scripts/idf.sh flash monitor
```

Public release build:

```bash
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
./scripts/prepare_web_firmware.sh
```

Public firmware must keep `CONFIG_WEC_DEVELOPMENT_BUILD` disabled and show
`微笺屏`. Private/local builds may enable it and show `微笺（开发版）`.

## Repository Map

- `main/`: ESP32 firmware
- `web/`: Web Serial installer, configuration page, firmware manifest, behavior contract
- `integrations/openclaw/`: `@openbrt/weclawbotctl` and OpenClaw plugin
- `integrations/hermes/`: Hermes plugin
- `runtime/`: rules-first curator runtime examples
- `docs/`: architecture, protocols, reliability notes, privacy boundary, release boundary

The current behavior contract is `web/firmware-contract.json`. Website
simulation and release tooling should follow that file instead of copying
firmware behavior by hand.

## Privacy Boundary

This public repository may contain firmware source, the installer/config page,
public release artifacts, user docs, Agent plugin source, and audited behavior
contracts.

It must not contain official cloud deployment code, private gateway logs,
WeChat tokens, Wi-Fi passwords, model API keys, MQTT broker admin credentials,
Tencent Cloud resource IDs, raw user data, or private prompts.

Before publishing, read
[`docs/public-release-boundary.md`](docs/public-release-boundary.md) and run
[`docs/project-release-checklist.md`](docs/project-release-checklist.md).

## Links

- Install/configure: <https://openbrt.github.io/weclawbot/web/>
- Product experience: <https://weclawbot.link/>
- npm package: <https://www.npmjs.com/package/@openbrt/weclawbotctl>
- Releases: <https://github.com/openbrt/weclawbot/releases>

## License

See [LICENSE](LICENSE).
