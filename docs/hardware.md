# Hardware Notes

Target board:

- Waveshare ESP32-S3-RLCD-4.2
- ESP32-S3 with PSRAM
- 4.2-inch reflective LCD, 400 x 300
- 16 MB flash, DIO, 80 MHz

Known button mapping:

| Board button | GPIO | Firmware behavior |
| --- | --- | --- |
| KEY / left | GPIO18 | Previous page, long press clears text notes |
| BOOT / right | GPIO0 | Next page, long press clears text, photo, and WeChat state |

Manual download mode:

1. Unplug USB-C.
2. Hold `BOOT`.
3. Plug USB-C back in.
4. Release `BOOT` after 1 to 2 seconds.

