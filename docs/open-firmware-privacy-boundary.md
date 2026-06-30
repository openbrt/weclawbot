# Open Firmware Privacy Boundary

WeClawBot publishes the firmware side so users can inspect the code that runs
inside their private environment. The hosted cloud implementation may remain
closed, but the device behavior must be auditable.

## Public Protocol Use

The firmware uses Tencent's public ilink bot protocol for QR login, message
polling, and replies. Publishing firmware-side ilink calls is not publishing a
secret protocol. The audit question is how the firmware uses the protocol:

- where the bot token is stored;
- whether the token is ever sent to non-ilink services;
- which message fields are published to the gateway/Agent event channel;
- what still works when the curator is unavailable.

## What Must Be Inspectable

The open firmware should make these boundaries clear in source code:

- WeChat QR login happens on the ESP32.
- The ESP32 owns `getupdates`; the cloud curator is not a message receiver.
- The WeChat bot token is stored locally in NVS and is used only for ilink
  requests.
- Gateway/Agent events exclude the WeChat bot token, QR credential, and message
  cursor.
- `wechat_id = "u_" + ilink_bot_id` is a screen identity, not an authorization
  secret.
- `sender_ref` is used so the device can reply to the correct WeChat peer.
- Wi-Fi credentials, WeChat credentials, local notes, idle photos, and optional
  provider tokens are local device state.
- Firmware updates through the web installer do not intentionally overwrite the
  NVS credential area.

## Data Sent To The Gateway Or Agent

For text and WeChat-provided voice transcripts, the device may send:

- event id;
- event kind;
- candidate text;
- `wechat_id` / `screen_id`;
- `sender_ref`;
- current screen note text and render revision;
- selected Agent mode and bounded device capability metadata.

For image/file paths, the device may send short-lived media metadata required
for the gateway or paired Agent to fetch and process the item. The cloud
service still must not receive the WeChat bot token.

## What Can Stay Closed

The public firmware does not need to include the hosted WeClawBot cloud
implementation:

- model routing;
- prompt and skill evolution pipeline;
- OCR / PDF / DOCX / PPTX / XLSX processing;
- server logs and eval ledgers;
- Tencent SCF deployment code and resource identifiers;
- abuse controls and billing logic.

The public contract only needs to document the MQTT/WSS event and control
schemas well enough for users to pair or self-host another compatible Agent.

## Release Rule

Before publishing, the repository must not include:

- Wi-Fi credentials;
- WeChat bot tokens, QR tokens, cursors, or context tokens;
- model provider API keys;
- real SCF URLs or cloud resource identifiers;
- raw device logs containing private messages;
- the local `history/` research archive;
- generated firmware binaries unless they are intentionally attached to a
  release artifact.
