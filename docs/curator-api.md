# Legacy Curator Endpoint Contract

This file is kept for old links. The product architecture has moved from a
user-configured HTTP curator URL to two explicit ownership modes:

- official mode: WeChat iLink events are owned by the ESP32 and routed through
  the official Agent over the WeClawBot gateway and MQTT/WSS;
- BYOA mode: the user's own Agent pairs with a six-digit code and controls the
  screen through MQTT/WSS.

Normal users should not type a curator URL, open a firewall port, or expose an
Agent callback endpoint from the configuration page.

The legacy HTTP curator field remains in firmware only as a transition and
debugging path. New integrations should use:

- [agent-direct-control-protocol.md](agent-direct-control-protocol.md) for
  MQTT pairing, `activity`, `screen_document`, and `screen_clear`;
- [byoa-reliability-analysis.md](byoa-reliability-analysis.md) for reliability
  and failure-mode discussion;
- [open-firmware-privacy-boundary.md](open-firmware-privacy-boundary.md) for
  what the public firmware may send to the gateway or an Agent.

Firmware-side legacy rules remain:

- the URL must use `http://` or `https://`;
- WeChat login credentials stay on the device;
- the endpoint must never receive the WeChat bot token, QR credential, message
  cursor, or Wi-Fi password;
- rendered bitmap pages, when used, must fit the hardware contract described in
  `web/firmware-contract.json`.
