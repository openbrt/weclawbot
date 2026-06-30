# USB MSC Product Disk

WeClawBot should be able to introduce and configure itself without depending on
the user finding the correct website first. When the board is plugged into a
computer, the firmware should expose a small USB Mass Storage Class disk named
`WECLAWBOT`. That disk is part of the product surface, not a development-only
artifact.

## Goals

- Provide offline help and product orientation from the device itself.
- Include browser entry pages for firmware installation, Wi-Fi configuration,
  official/custom Agent mode, and troubleshooting.
- Keep the public firmware auditable: the disk must not expose WeChat tokens,
  Wi-Fi credentials, API keys, local note cache, or cloud implementation details.
- Preserve the browser-based setup flow used by VoxStick-like projects: users
  open an HTML page, then the page talks to the device through a browser USB
  interface.

## Disk Contents

Initial disk contents should be generated from the public repository at build
time:

- `README.html`: quick start, safety notes, and device status meanings.
- `README.zh-CN.html`: Chinese quick start.
- `install.html`: ESP Web Tools firmware installation and update entry.
- `config.html`: Web Serial/WebUSB configuration UI for Wi-Fi, official/custom
  Agent mode, and basic device state.
- `help/`: static troubleshooting pages.
- `firmware/manifest.json`: local firmware manifest when the image bundle fits.
- `firmware/`: optional local firmware binaries for offline flashing.
- `VERSION.txt`: firmware version, board target, build date, and public source
  URL.

The pages may link to `https://weclawbot.link/` for the newest online docs, but
the mounted disk must remain useful without network access.

## Entry Recovery

Users will forget the website after the device has been running for a while.
The product must therefore provide multiple memory-free recovery paths:

- The USB MSC disk is the primary recovery path. Plugging the device into a
  computer reveals the `WECLAWBOT` drive and its start page.
- The firmware should show `weclawbot.link` on USB configuration and recovery
  screens.
- The WeChat bot should reply to `帮助`, `官网`, `/help`, and similar messages
  with the product URL and the USB-disk setup path.
- Printed packaging and public docs may mention the URL, but the product must
  not depend on packaging being kept.

## Firmware Architecture

ESP-IDF 5.5 includes a TinyUSB composite device example that supports MSC plus
CDC serial on ESP32-S3. WeClawBot should follow that direction instead of using
MSC alone, because the existing local configuration page already speaks the
line-based `WEC:` protocol over USB serial.

Recommended device functions:

- MSC: exposes the `WECLAWBOT` help/install/config disk.
- CDC ACM: exposes the same `WEC:` configuration protocol currently carried by
  USB-Serial-JTAG/UART.
- ROM download mode remains unchanged for recovery flashing.

The current firmware uses USB-Serial-JTAG for configuration. Migrating to the
TinyUSB composite stack will require adapting `SerialConfig` to read and write
through TinyUSB CDC while keeping UART fallback for development logs.

## Partition Plan

Do not reuse the existing `storage` SPIFFS partition. That partition holds local
note state, screen-frame cache, idle-photo cache, and other runtime data.

Add a dedicated FAT partition, for example:

```text
usbdrive, data, fat, , 0x180000,
```

The exact size depends on whether firmware binaries are bundled locally:

- Help/config pages only: 512 KiB to 1 MiB is enough.
- Pages plus split firmware binaries: 2 MiB or more is safer.
- Full merged firmware image: usually too large to be worth bundling on every
  build unless the app partition budget is adjusted deliberately.

The first implementation should treat the MSC disk as a generated product disk.
Runtime writes from the host are not required. If writable behavior is enabled
later, the firmware must avoid concurrent filesystem access while the host has
the disk mounted.

## Build Pipeline

The build should generate disk contents deterministically:

1. Build firmware.
2. Run `scripts/prepare_web_firmware.sh`.
3. Render/copy static pages from `web/` and `docs/` into a staging directory.
4. Generate `VERSION.txt`.
5. Create a FAT image for the `usbdrive` partition.
6. Flash the app and the `usbdrive` image as separate artifacts.

The release assets should include the application binaries, web installer
bundle, and the product-disk image. The public repository should not include
private cloud endpoints beyond the stable public product URL, and it must never
include credentials.

## Open Questions

- Whether `install.html` should embed local firmware binaries or always fetch
  the newest public release from GitHub/weclawbot.link.
- Whether configuration should continue using Web Serial over TinyUSB CDC or
  move to WebUSB for a cleaner browser permission flow.
- Whether macOS/Windows should see the disk as read-only at the USB protocol
  level, or as a writable FAT disk whose writes are ignored/reset by firmware.
- How much flash should be reserved for `usbdrive` without constraining future
  app growth, fonts, photo rendering, and Layout VM experiments.
