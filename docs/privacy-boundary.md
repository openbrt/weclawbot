# Firmware Privacy Boundary

WeClawBot publishes the device firmware so users can inspect the code that runs
inside their home or office.

The firmware owns:

- Wi-Fi configuration stored in ESP32 NVS.
- WeChat QR login state stored in ESP32 NVS.
- The `getupdates` polling loop.
- Local note and idle-photo cache.
- The configurable curator endpoint and optional provider credentials.

The curator endpoint receives only the message payload needed to prepare a
screen update. It does not receive the WeChat bot token from firmware.

Cloud implementation, deployment scripts, server logs, and provider secrets are
outside the public firmware repository.

