# Firmware Privacy Boundary

This compatibility document points old links to the current privacy boundary:

- [open-firmware-privacy-boundary.md](open-firmware-privacy-boundary.md)
- [architecture.md](architecture.md)
- [agent-direct-control-protocol.md](agent-direct-control-protocol.md)

The short version:

- ESP32 owns Wi-Fi credentials, WeChat QR login, iLink `getupdates`, WeChat bot
  token storage, local notes/photos, display refresh, and hardware buttons.
- Official cloud and BYOA Agents may receive bounded event metadata and screen
  capability information, but not WeChat bot tokens, QR credentials, message
  cursors, Wi-Fi passwords, or local NVS secrets.
- BYOA mode ignores WeChat input. The user's Agent controls the screen over
  MQTT/WSS using `activity`, `screen_document`, and `screen_clear`.
- Public firmware describes hardware and protocol boundaries. Agent skills own
  layout quality, preview delivery, file handling, and user-facing workflow.

The public repository does not contain official cloud deployment secrets,
private gateway logs, model API keys, MQTT broker admin credentials, or user
data.
