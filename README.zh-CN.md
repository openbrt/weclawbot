# WeClawBot / 微笺屏

[固件安装 / 设备配置](https://openbrt.github.io/weclawbot/web/) ·
[固件 Release](https://github.com/openbrt/weclawbot/releases) ·
[官网体验](https://weclawbot.link/) ·
[English README](README.md)

WeClawBot 是面向 Waveshare ESP32-S3-RLCD-4.2 的开源桌面屏固件和 Agent
连接体系。它把一块 400 x 300 黑白反射屏变成常驻的微笺屏：留言、提醒、
照片、状态卡、读书摘记、任务进度和 Agent 生成的仪表盘，都可以安静地留在
桌面、床头或餐桌边。

产品承诺只有一句：

> 把有用的东西可靠、安静地放到这块小屏上。

## 两种接管方式

- **WeClawBot 官方**：普通用户刷固件、配 Wi-Fi、扫屏幕上的微信二维码。
  官方智能体理解用户意图，渲染成适合 4.2 寸黑白屏的画面，再通过 MQTT/WSS
  推回真机。
- **BYOA / 自定义智能体**：高级用户在配置页选择“自定义智能体”。屏幕显示
  六位配对码，OpenClaw、Hermes、Codex、Claude Code、Gemini CLI、OpenCode
  或任意脚本安装 `@openbrt/weclawbotctl` 后即可接管屏幕。

用户不需要填写网关 URL，不需要开放服务器端口，也不需要把微信凭证交给
Agent。ESP32 保管 Wi-Fi、微信登录、本地状态、按键、显示刷新和硬件边界；
Agent 负责意图理解、版面、预览、文件处理、模型调用和用户对话。

## 固件安装与设备配置

打开公开安装/配置页：

```text
https://openbrt.github.io/weclawbot/web/
```

请使用桌面 Chrome 或 Edge。这个页面同时负责：

- 通过 ESP Web Tools 刷写固件；
- 通过 `WEC:` 串口协议配置 Wi-Fi 和接管方式。

首次刷机：按住 `BOOT`，插入 USB-C，等串口出现后松开 `BOOT`。普通升级不会
擦除 NVS，所以 Wi-Fi、微信登录、Agent 模式、本地微笺和照片会保留；只有主动
全清、擦除 flash，或刷入覆盖 NVS 的合并镜像时才会清掉配置。

刷完后：

1. 在配置页连接设备。
2. 保存 Wi-Fi。
3. 选择 **WeClawBot 官方** 或 **自定义智能体**。
4. 保存并重启。

官方模式会显示微信二维码；自定义智能体模式会显示六位配对码。

## 配对自己的 Agent

当屏幕显示 BYOA 配对码时，对自己的 Agent 说：

```text
Install @openbrt/weclawbotctl and connect to my WeClawBot screen with pairing code 123456.
```

把 `123456` 换成屏幕上的码。Agent 应安装 npm 包、绑定设备、运行在线检查，
并使用 `~/.config/weclawbot/agent-mqtt.json` 中的 MQTT 配置。

手动命令：

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl bind 123456 --name openclaw
weclawbotctl doctor --online
```

OpenClaw 用户继续执行：

```bash
weclawbotctl openclaw install
weclawbotctl openclaw doctor
```

稳定的 BYOA 控制命令：

```bash
weclawbotctl thinking --id "$task_id" --ttl 45
weclawbotctl preview /path/to/screen-document.json
weclawbotctl screen /path/to/screen-document.json
weclawbotctl idle --id "$task_id"
weclawbotctl clear
```

`screen` 发布像素并等待固件 `applied` 回执。预览图是产品的一部分，不只是
调试截图；它让用户和 Agent 在使用中共同调整密度、字体、分页和审美。

## 硬件

- 开发板：Waveshare ESP32-S3-RLCD-4.2
- 屏幕：4.2 寸黑白反射 LCD，400 x 300 横屏
- Flash：16 MB
- 传感器：SHTC3 温湿度
- 按键：
  - 左键短按：上一屏
  - 左键长按：清文字微笺
  - 右键短按：下一屏
  - 右键长按：全清

可见屏幕最多 5 屏：日历、照片、最多 3 页留言。左右键能很快翻完。

## 固件和 Agent 的分工

固件要稳定、无聊、可靠：

- Wi-Fi 和微信/iLink token 存在本机 NVS；
- 官方模式由 ESP32 自己轮询微信 `getupdates`；
- BYOA 模式忽略微信输入；
- 官方和 BYOA 都通过 MQTT/WSS 接收控制；
- 校验 `activity`、`screen_document` 和 `screen_clear`；
- 只在 400 x 300 的硬件边界里显示已经渲染好的像素；
- 普通升级保留本地状态。

Agent 要快速演进：

- 判断用户真正想表达什么；
- 决定忽略、追问、渲染、清屏还是更新状态；
- 做出适合黑白小屏的可读版面；
- 在可读和少分页之间平衡；
- 把效果预览发回用户正在使用的对话渠道；
- 从用户修正和审美偏好里持续改进，而不是要求用户频繁刷固件。

## 本地构建

项目使用 ESP-IDF 5.4 或更新版本。

开发构建：

```bash
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
./scripts/idf.sh flash monitor
```

公开 release 构建：

```bash
./scripts/idf.sh -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" build
./scripts/prepare_web_firmware.sh
```

公开版必须关闭 `CONFIG_WEC_DEVELOPMENT_BUILD`，屏幕左上角显示 `微笺屏`。
私有/本地开发版可以打开该选项，显示 `微笺（开发版）`。

## 仓库结构

- `main/`：ESP32 固件
- `web/`：Web Serial 安装页、配置页、固件 manifest、行为契约
- `integrations/openclaw/`：`@openbrt/weclawbotctl` 和 OpenClaw 插件
- `integrations/hermes/`：Hermes 插件
- `runtime/`：规则优先的整理 runtime 示例
- `docs/`：架构、协议、可靠性、隐私边界和公开发布边界

`web/firmware-contract.json` 是公开行为契约。官网仿真机和 release 流程应以
它为准，不要手写另一套按键、二维码和屏幕规则。

## 隐私边界

公开仓库可以包含固件源码、安装/配置页、公开 release 产物、用户文档、Agent
插件源码和经过审查的行为契约。

公开仓库不能包含官方托管服务部署代码、私有网关日志、微信 token、Wi-Fi 密码、
模型 API key、MQTT broker 管理密钥、腾讯云资源 ID、用户原始数据或私有提示词。

发布前先读 [docs/public-release-boundary.md](docs/public-release-boundary.md)，
并执行 [docs/project-release-checklist.md](docs/project-release-checklist.md)。

## 链接

- 安装/配置页：<https://openbrt.github.io/weclawbot/web/>
- 官网体验：<https://weclawbot.link/>
- npm 包：<https://www.npmjs.com/package/@openbrt/weclawbotctl>
- 固件 Release：<https://github.com/openbrt/weclawbot/releases>

## 许可证

见 [LICENSE](LICENSE)。
