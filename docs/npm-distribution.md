# WeClawBot npm 分发

目标是把高级用户的 Agent 接入做成跨平台安装：

- 普通用户仍只用配置页选择“官方智能体”或“自定义智能体”。
- 高级用户在自己的电脑或服务器安装 npm 包。
- npm 包提供通用 `weclawbotctl`，并同时携带 OpenClaw 插件元数据。
- Hermes、Codex、Claude Code、Gemini CLI、OpenCode、Shell 脚本都可以复用
  同一个本地 MQTT profile：`~/.config/weclawbot/agent-mqtt.json`。

## 发布前检查

```bash
cd integrations/openclaw/weclawbot-openclaw-plugin
npm install --package-lock-only
npm run check
npm pack --dry-run
```

`npm pack --dry-run` 应只包含源码、README、bin、lib、skills、workspace、
systemd 和测试文件，不应包含 `node_modules`、本地 MQTT 凭据或任何密钥。

## 发布

当前包名为 `@openbrt/weclawbotctl`，使用 openbrt organization scope 发布；`@openbrt/weclawbot` 主包名先保留给未来 SDK 或品牌级入口。

当前已发布版本：

```bash
npm view @openbrt/weclawbotctl version
# 0.1.22
```

当前源码包版本为 `0.1.22`，在 `0.1.15` 的 OpenClaw 直接任务自动思考态基础上，
新增 `weclawbotctl preview`，并让 `weclawbotctl screen` 与
`weclawbot_publish_screen_document` 返回 `preview.pages[].path`。Agent 应检查
这些 PNG；在 OpenClaw 中，`0.1.19` 起默认写入 preview manifest，并由 hook 在
当前 run 收尾时通过 TG/UI 消息渠道回传。其他 Agent 仍应通过自己的媒体通道把
PNG 发给用户，作为上屏效果的可见证明。`0.1.17`
曾将旧的命令嗅探式自动附件预览默认关闭；`0.1.19` 改为 publish 层 manifest
确定性投递。
`0.1.18` 进一步说明效果图交付的学习目标：让 Agent 从用户反馈中持续贴近
用户的审美、阅读密度、字体偏好和分页习惯。
`0.1.19` 增加确定性预览投递实验：`screen` 成功上屏后写入 preview manifest，
OpenClaw hook 在当前 run 的时间窗内扫描 manifest 并把 PNG 通过当前会话渠道
发回用户，覆盖 Agent 通过 Python/脚本间接调用 CLI 的情况。
`0.1.20` 明确 mono1 位语义和默认首屏视觉基线：packed bit `1` 是黑墨，bit
`0` 是白纸；普通首屏和卡片默认应是白底黑字，避免 Agent 初次生成黑底白字。
`0.1.22` 增加重配对后的旧 Agent 快速失败：OpenClaw direct turn 只要提到
上屏/屏幕，会先做 MQTT online owner 检查；如果本地凭据已被云端撤销，hook
在模型运行前 block，返回 `credential_revoked_or_not_current_owner`，不再继续
生成脚本、图片或报告“已上屏”。CLI doctor 也支持 `--timeout seconds`，便于
Agent 在昂贵工作前做短超时预检。

已清理/迁移的早期包名：

- `weclawbot-openclaw-plugin@0.1.0`：deprecated，提示迁移到
  `@openbrt/weclawbotctl`。
- `@ccqqbb_cb/weclawbot-openclaw-plugin@0.1.0`：deprecated，提示迁移到
  `@openbrt/weclawbotctl`。
- `weclawbotctl@0.1.0`：deprecated，提示迁移到
  `@openbrt/weclawbotctl`。

这些早期包名若要彻底 unpublish，需要 npm 2FA OTP；当前已做软清理，用户安装
时会看到迁移提示。

```bash
cd integrations/openclaw/weclawbot-openclaw-plugin
npm login
npm publish --access public
npm view @openbrt/weclawbotctl version
```

## 用户安装

全局安装通用 CLI：

```bash
npm install -g @openbrt/weclawbotctl
weclawbotctl bind 123456 --name user-agent
weclawbotctl doctor --online --timeout 8
```

无需全局安装的一次性用法：

```bash
npm exec --package @openbrt/weclawbotctl -- weclawbotctl bind 123456 --name user-agent
npm exec --package @openbrt/weclawbotctl -- weclawbotctl doctor --online --timeout 8
```

OpenClaw 可直接按 npm spec 安装插件：

```bash
weclawbotctl openclaw install
weclawbotctl openclaw doctor
```

OpenClaw 插件工具要求 OpenClaw `2026.6.9` 或更新版本。`openclaw install`
会安装并启用 `weclawbot` 插件；之后需要重启 OpenClaw gateway 或应用，让
新工具进入 agent 上下文。`openclaw doctor` 会检查版本、插件安装、
`contracts.tools` 诊断、WeClawBot hook 权限和本地 gateway 可达性；如果用户的 OpenClaw 使用
自签 WSS gateway，它只给出 `NODE_EXTRA_CA_CERTS` / 证书 SAN 修复提示，
不会自动改写用户的 OpenClaw 证书。

非交互 SSH、systemd 或部分 npm 全局安装环境可能没有把 OpenClaw 放进
`PATH`。`weclawbotctl openclaw ...` 会优先探测同一个 npm global `bin`
目录下的 `openclaw`；仍找不到时可显式传入：

```bash
weclawbotctl openclaw doctor --bin /path/to/openclaw
```

旧无 scope 包名只保留迁移提示；新文档和用户入口只使用
`@openbrt/weclawbotctl`。

给 Agent 的一句话：

```text
Install @openbrt/weclawbotctl and connect to my WeClawBot screen with pairing code 123456.
```

Agent 应自行阅读 npm README，完成 install、bind、doctor，并在工作中使用
`weclawbotctl thinking` / `idle` / `screen` / `clear`。清屏必须走
`screen_clear`，不能用纯黑、纯白或空白 `screen_document` 模拟。`screen` 文档
的 `mono1` 打包规则是 `1=黑墨`、`0=白纸`；普通内容默认白底黑字，只有用户明确
要求反白、夜间或黑底海报时才用大面积黑底。
直接上屏前必须先跑 `weclawbotctl doctor --online --timeout 8` 或
`weclawbot_status online:true`；如果返回
`credential_revoked_or_not_current_owner`，说明屏已切到另一个 Agent，旧 Agent
应立即停止并提示重新配对。
