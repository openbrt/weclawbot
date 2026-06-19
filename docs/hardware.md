# Hardware Notes

The Waveshare ESP32-S3-RLCD-4.2 has two user buttons exposed to firmware.
Facing the screen, the three top controls are KEY, PWR, and BOOT from left to
right. PWR is handled by the board power circuit and is not a normal ESP32 GPIO.

| Name | GPIO | Active Level | WeClawBot use |
| --- | --- | --- | --- |
| KEY | GPIO18 | Low | Short press: previous page; long press 3s: clear text notes |
| BOOT | GPIO0 | Low | Short press: next page; long press 5s: clear photo, text, and WeChat login |

RLCD pins used by the driver:

| Signal | GPIO |
| --- | --- |
| DC | GPIO5 |
| CS | GPIO40 |
| SCK | GPIO11 |
| MOSI | GPIO12 |
| RST | GPIO41 |
| TE | GPIO6 |

The firmware drives the display in 400 x 300 landscape mode.

Board sensor:

| Device | I2C Address | SDA | SCL | WeClawBot use |
| --- | --- | --- | --- | --- |
| SHTC3 temperature/humidity sensor | `0x70` | GPIO13 | GPIO14 | Calendar home and persistent status bar temperature/humidity |

Battery sensing:

| Signal | GPIO / ADC | Notes | WeClawBot use |
| --- | --- | --- | --- |
| Battery voltage | GPIO4 / ADC1_CH3 | Board example documents battery voltage through a 3x divider. USB power can still make this rail look like a lithium cell, so `CONFIG_WEC_BATTERY_INSTALLED` gates whether the UI treats it as a real battery. | Top status battery/no-battery icon |
