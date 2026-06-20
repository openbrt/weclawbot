# WeClawBot

基于 Waveshare ESP32-S3-RLCD-4.2 的微信连接 AI、自我进化桌面屏开源固件。

[English README](README.md) | [官网体验](https://weclawbot.link/) | [安装和配置页面](https://openbrt.github.io/weclawbot/web/)

![WeClawBot 真机效果](docs/assets/weclawbot-device-hero-v0.1.46.png)

## 这是什么

WeClawBot 把一块 4.2 寸黑白反射屏变成安静、常驻、没有手机干扰的微笺屏：

- 用微信扫码连接 bot。
- 在微信里发送留言、清单、提醒或照片，设备把内容整理成桌面微笺、
  日历页或黑白相框。
- 微信端可继续发送“修改为...”“清屏”等指令调整屏上内容。
- 新留言会先即时呈现，随后和日历、已设置的黑白照片相框轮流显示；多页留言在
  留言环节内自动翻页，不让屏幕长期被单一内容占住。
- 无当前微笺时，屏幕保留日历、时钟、室内温湿度、Wi-Fi 和插电/电池状态。
- 普通刷固件不会清 Wi-Fi、微信连接、留言和云端整理 URL。
- 安装、刷固件、配置 Wi-Fi 都走浏览器 USB 页面。

公开固件源码的目的，是让用户能检查进入自己隐私环境的设备端代码。
云端整理服务实现不在本仓库内。

## 硬件

- 开发板：Waveshare ESP32-S3-RLCD-4.2
- 主控：ESP32-S3，带 PSRAM
- 屏幕：4.2 寸反射 LCD，400 x 300
- USB：ESP32-S3 USB Serial/JTAG
- Flash：16 MB，DIO，80 MHz

固件 `0.1.47` 的按键定义：

| 按键 | 短按 | 长按 |
| --- | --- | --- |
| 左键 | 上一屏 | 3 秒后清文字留言 |
| 右键 | 下一屏 | 5 秒后全清：文字、照片和微信登录态 |
| 中间电源键 | 预留给硬件 | 开机 / 关机 |

公开固件屏幕左上角显示 `微笺屏`。本地开发版可打开
`CONFIG_WEC_DEVELOPMENT_BUILD`，左上角会显示 `微笺（开发版）`。

## 安装

打开浏览器安装页：

https://openbrt.github.io/weclawbot/web/

建议使用 Chrome 或 Edge，因为页面需要 Web Serial / WebUSB。

如果还没有硬件，可以先打开官网体验：

https://weclawbot.link/

手动进入下载模式：

1. 拔掉 USB-C。
2. 按住 `BOOT`。
3. 插回 USB-C。
4. 等 1 到 2 秒后松开 `BOOT`。

普通升级不会擦除 NVS，所以 Wi-Fi 和微信绑定会保留。只有你主动执行全清，
或用刷写工具选择擦除设备时，才会清掉配置。

## 本地构建

项目使用 ESP-IDF。

```bash
./scripts/idf.sh build
./scripts/idf.sh -p /dev/cu.usbmodem21101 flash
./scripts/prepare_web_firmware.sh
```

浏览器安装页使用的固件文件会生成到 `web/firmware/`。

## 隐私边界

ESP32 端负责微信扫码登录和 `getupdates` 轮询。配置的云端整理 endpoint 只接收
整理屏幕内容所需的消息内容，不接收微信 bot token。

用户可以在本地配置页自行填写 provider、endpoint 和 API token。WeClawBot 默认
服务只是可选 provider 之一，不是固件运行的隐藏前提。

## 本仓库包含什么

- ESP32 固件源码。
- 浏览器刷写和本地配置页面。
- 用户指引文档。
- 供安装页使用的预构建固件文件。

本仓库不包含：

- WeClawBot 云端整理服务实现。
- 服务器日志或部署脚本。
- 腾讯云、COS、SCF 或模型 provider 的私有凭据。

## 许可证

见 [LICENSE](LICENSE)。
