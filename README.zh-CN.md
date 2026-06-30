# WeClawBot / 微笺屏

WeClawBot 是面向 Waveshare ESP32-S3-RLCD-4.2 的开源反射屏固件。它把
400 x 300 黑白常显屏变成桌面、床头、餐桌上的微笺：留言、提醒、照片、
状态卡和 Agent 生成的仪表盘都可以安静地留在屏上。

[English README](README.md) | [官网体验](https://weclawbot.link/) |
[安装和配置页面](https://openbrt.github.io/weclawbot/web/)

![WeClawBot 真机效果](docs/assets/weclawbot-device-hero-v0.1.46.png)

## 两种连接形态

- **WeClawBot 官方模式**：普通用户在屏幕上扫微信二维码。官方智能体理解
  用户意图，整理成适合 4.2 寸黑白屏的版面，再通过 MQTT/WSS 推回设备。
- **BYOA / 自定义智能体模式**：高级用户在配置页选择“自定义智能体”。屏幕
  显示六位配对码，OpenClaw、Hermes、Codex、Claude Code、Gemini CLI、
  OpenCode 或任意本地脚本安装 `@openbrt/weclawbotctl` 后即可接管屏幕。
  用户不需要填写网关 URL、开放端口或交出微信凭证。

固件只负责 Wi-Fi、显示刷新、按键、本地状态和硬件边界。智能体负责意图理解、
版面审美、字体、分页、预览和用户对话。这样固件保持稳定，Agent skill 和
用户习惯可以快速演进。

## 硬件

- 开发板：Waveshare ESP32-S3-RLCD-4.2
- 屏幕：4.2 寸反射 LCD，400 x 300 横屏
- 主控：ESP32-S3，带 PSRAM
- 按键：
  - 左键 / KEY：短按上一屏，长按 3 秒清文字
  - 右键 / BOOT：短按下一屏，长按 5 秒全清

## 浏览器安装与配置

打开：

```text
https://openbrt.github.io/weclawbot/web/
```

建议使用桌面 Chrome 或 Edge，因为页面需要 Web Serial / WebUSB。

手动进入下载模式：

1. 拔掉 USB-C。
2. 按住 `BOOT`。
3. 插回 USB-C。
4. 等串口出现后松开 `BOOT`。

普通升级不会擦除 NVS，所以 Wi-Fi、微信登录、Agent 模式、本地留言和照片会
保留。只有主动全清、擦除 flash，或刷入覆盖 NVS 的合并镜像时才会清配置。

配置页只暴露用户能理解的选择：

- **WeClawBot 官方**：保存 Wi-Fi 和模式，重启后扫码使用。
- **自定义智能体**：保存 Wi-Fi 和模式，重启后读取屏幕左下的六位配对码，
  然后告诉自己的 Agent：

```text
Install @openbrt/weclawbotctl and connect to my WeClawBot screen with pairing code 123456.
```

把 `123456` 替换成屏上的配对码。Agent 应安装 npm 包、绑定设备、运行
`weclawbotctl doctor --online`，并使用
`~/.config/weclawbot/agent-mqtt.json` 中的 MQTT 配置。

## 本地构建

项目使用 ESP-IDF 5.4 或更新版本：

```bash
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
./scripts/idf.sh flash monitor
```

公开 release 构建必须关闭 `CONFIG_WEC_DEVELOPMENT_BUILD`，显示 `微笺屏`。
准备公开安装包时合并 release defaults：

```bash
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
./scripts/prepare_web_firmware.sh
```

私有/本地开发版可以打开 `CONFIG_WEC_DEVELOPMENT_BUILD`，屏幕左上角会显示
`微笺（开发版）`，但不要把这类内部版本号写成公开用户承诺。

## Agent CLI 和插件

BYOA 路径通过 npm 包分发：

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl bind 123456 --name openclaw
weclawbotctl doctor --online
```

一次性使用也可以：

```bash
npm exec --package @openbrt/weclawbotctl -- weclawbotctl bind 123456 --name codex
```

OpenClaw 用户可继续：

```bash
weclawbotctl openclaw install
weclawbotctl openclaw doctor
```

插件提供状态、清屏、thinking 活动、screen document 校验、预览图和发布工具。
`screen` 默认等待固件 `applied` 回执；预览图是用户和 Agent 共同改进审美、
密度、字体和分页节奏的反馈面。

## 隐私边界

ESP32 端负责微信扫码登录、iLink `getupdates`、微信 token 存储、Wi-Fi、
本地微笺、照片缓存、显示刷新和按键。官方云端或用户 Agent 不应拿到微信
bot token、Wi-Fi 密码、二维码凭证或消息游标。

公开仓库包含：

- ESP32 固件源码
- Web Serial 安装/配置页
- 用户文档
- Agent 插件源码和 skill 文档
- `web/firmware-contract.json` 行为契约

公开仓库不包含：

- 官方云端/网关私有实现
- 微信 token、用户日志、Wi-Fi 密码
- DeepSeek / 模型 API key
- MQTT broker 管理密钥
- 腾讯云、COS、SCF 私有资源 ID

更多背景见 [docs/current-progress.md](docs/current-progress.md)。

## 公开发布边界

发布到公开仓库前，先读
[docs/public-release-boundary.md](docs/public-release-boundary.md)。这是
`openbrt/weclawbot` 的 canonical 规则：哪些文件可以公开、哪些必须留在
私有工作区、公开固件产物必须怎样构建，都以它为准。

## 许可证

见 [LICENSE](LICENSE)。
