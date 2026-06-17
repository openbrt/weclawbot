# Curator Endpoint Contract

The firmware can call a configurable HTTP curator endpoint. The default hosted
service is one provider. Users may configure their own endpoint from the local
browser page.

The endpoint is expected to return a decision that tells the firmware whether to
update the screen, ignore the message, clear content, or set an idle photo.

The public firmware repository intentionally documents the boundary rather than
shipping the private cloud implementation.

Firmware-side rules:

- The endpoint must use `http://` or `https://`.
- The URL, provider, model, and optional token are configured over USB serial.
- WeChat login credentials stay on the device.
- Screen-rendered bitmap pages may be returned by the endpoint and cached by the
  device together with text context.

