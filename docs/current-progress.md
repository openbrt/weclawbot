# WeClawBot 当前进展记录

记录日期：2026-06-27

这份文档用于交接当前项目状态。它不是最终产品说明，而是把已经形成的
产品判断、实现边界、已验证路径和待处理问题集中放在一个入口，方便后续
同学继续讨论和开发。

## 项目定位

WeClawBot / 微笺屏是基于 Waveshare ESP32-S3-RLCD-4.2 的开源固件项目。
公开仓库只发布固件源码、安装配置页面、用户指引和可审计的行为契约；
官方云端整理、微信网关、部署密钥、用户数据和模型提示词不公开。

产品方向已经从“微信消息直接摘要成一张贴纸”调整为：

- 普通用户：从官网或 GitHub 烧写固件、配网、微信扫码，使用
  WeClawBot 官方智能体。
- 高级用户：在配置页选择“自定义智能体”，通过绑定码让自己的
  Hermes / OpenClaw / 其他 Agent 接管屏幕。
- 固件只声明硬件边界和安全约束，不替用户的智能体决定内容审美。
- 屏幕能力是一个统一能力：**发到屏上**。它可以来自微信，也可以来自
  用户自己的 Agent、定时任务或自动化工具。
- 普通用户主路径是 **iLink 到屏，屏通过 MQTT/WSS 与官方网关/Agent 通信**；
  BYOA 主路径是用户自己的 Agent 通过 MQTT 接管屏幕，微信入口直接忽略。
  旧的 HTTP `curator_url` 只作为过渡实现和调试入口，不再作为用户可见
  配置或长期架构边界。

相关文档：

- [architecture.md](architecture.md)
- [agent-direct-control-protocol.md](agent-direct-control-protocol.md)
- [byoa-reliability-analysis.md](byoa-reliability-analysis.md)
- [official-site-firmware-sync.md](official-site-firmware-sync.md)
- [open-firmware-privacy-boundary.md](open-firmware-privacy-boundary.md)
- [public-release-boundary.md](public-release-boundary.md)
- [project-release-checklist.md](project-release-checklist.md)
- [npm-distribution.md](npm-distribution.md)

## 固件当前状态

目标硬件：

- Waveshare ESP32-S3-RLCD-4.2
- 400 x 300 横屏 RLCD
- SHTC3 温湿度传感器
- 可插电使用，也可安装电池
- USB Serial/JTAG 用于本地配置和调试

已经具备：

- Wi-Fi 配网和串口配置协议 `WEC:*`
- DHCP hostname 设置为 `WeClawBot`
- 微信 iLink 登录、二维码状态、`getupdates` 接收消息
- 固件内置官方服务入口；下一版不再要求用户配置整理 URL
- 当前代码仍保留 legacy HTTP `curator_url` / `/byoa` bootstrap 路径
- 配置页只显示“官方智能体 / 自定义智能体”，保存时发送 `agent_mode`
  而不是 URL
- Web 安装/配置页顶部不再硬写固件号；连接真机后显示设备上报版本
- 本地状态栏：Wi-Fi、供电/电池相关状态
- SHTC3 温湿度读取，并在日历兜底页显示
- 日历兜底页、留言页、照片页的基础显示
- 红方块桌宠：空闲、思考等状态的黑白屏适配
- 左右键短按翻页，长按清理状态
- 云端像素图下发后由固件显示，不再依赖板端字体完成复杂排版
- BYOA 模式下接收 Agent 的 `activity` 和 `screen_document`
- 公开固件与官网仿真机同步的 `web/firmware-contract.json` 已补齐

已知版本问题：

- 源码、CMake 和 firmware contract 当前公开版本为 `0.1.77`；Web 安装页
  连接真机后显示设备上报版本。
- `scripts/prepare_web_firmware.sh` 已重新生成 `web/firmware/`。
- 下一次正式发布前仍必须核对 release 产物和安装页 manifest。
- 固件版本现在要按渠道看待：私有/本地开发版可以继续推进内部版本并显示
  `微笺（开发版）`；公开版 release 必须关闭 `CONFIG_WEC_DEVELOPMENT_BUILD`，
  显示 `微笺屏`，并以公开 tag/release 的 manifest 为准。公开配置页顶部不应
  把私有开发固件号当产品文案承诺，连接真机后再显示设备上报版本。

开发版显示规则：

- 开发版左上角应显示 `微笺（开发版）`
- 公开版左上角应显示 `微笺屏`
- 当前曾出现右括号缺失问题，需要作为发布前 UI 检查项保留

## 官方云端整理现状

当前官方云端整理能力已经暴露出明显问题：

- 对购物清单、待办、照片、诗词、家庭留言等语义归并不稳定
- 容易过度摘要，破坏用户原文中的语气和关系信息
- 对多列、一屏/三屏、照片相框、轮播等显示策略缺少稳定审美
- 提示语和微信回复有时与真实屏幕生命周期不一致

当前结论：

- 不再把“小模型 + 大模型 + COS 规则沉淀”作为唯一主路径。
- 官方智能体可以继续服务普通用户，但高级用户必须能带自己的 Agent。
- 网关只负责连接、配对、路由、鉴权、低成本在线通道；普通官方模式下
  微信消息由设备通过 MQTT/WSS 发布给官方 Agent，而不是长期走可配置
  HTTP URL。BYOA 配对后微信入口不再进入控制面。
- 内容理解、排版策略和个性化行为应尽量交给用户选择的 Agent。

## BYOA / Agent 直控

已经形成的设计：

1. 用户在配置页选择“自定义智能体”。
2. 固件使用内置官方服务入口做 bootstrap，不暴露或要求填写 URL。
3. 屏幕显示六位 `AI智能体绑定码`，有效期 10 分钟。
4. 用户在 Hermes / OpenClaw 插件里输入绑定码。
5. 网关生成 Agent 与 Device 两套最小权限 MQTT 凭证。
6. 设备连接 MQTT/WSS，发布在线状态和 `device_context`；BYOA 下不发布微信入口事件。
7. Agent 可以发送：
   - `activity`：显示/清除“思考中的桌宠”
   - `screen_document`：发送 1 到 3 页 1-bit bitmap 内容区

MQTT 语义：

- TLS / WSS
- 设备上行 `events/status` 采用 QoS 0 直接写入；官方模式微信文本在固件侧有
  有界 RAM 暂存和重试，BYOA 模式不接收、不暂存、不转发微信入口
- Agent 下行 `control` 仍按在线即时投递设计
- clean session，无离线队列
- 不使用 retained message
- Agent 离线期间不堆积未来命令
- 推荐最小刷新间隔当前为 60 秒

### 2026-06-27 OpenClaw / Telegram 路径排查

100 服务器上两个任务入口的真实问题已经追到：

- OpenClaw 管理 UI 那次能上屏，是因为 agent 临时写了 Python/PIL，把内容渲成
  368x206 mono1 像素文档，再调用
  `/home/csc/.npm-global/bin/weclawbotctl screen <doc.json>`。这条路径本身正确，
  但当时没有沉淀成插件工具和 workspace 规则。
- Telegram bot 失败，是因为它读到旧的 `~/.openclaw/extensions/weclawbot`
  私有插件 `0.1.0`：插件未在 allowlist 中、只提供 validator、不提供 publish
  tool，并且旧 skill 把 `agent_transport.available=false` 误解释为“直推不可用”。
  它还把 WeClawBot 误联想到 OpenClaw Canvas。
- OpenClaw gateway 同时在刷 Telegram native approval 的 TLS 错误：先是
  `self-signed certificate`，信任本地 CA 后又暴露出证书缺少
  `127.0.0.1` SAN。
- npm 插件 `0.1.4` 一开始也缺 `openclaw.plugin.json#contracts.tools`，
  OpenClaw `plugins doctor` 因此报告 “plugin must declare contracts.tools”。
  另外 curator systemd service 写死旧扩展目录，npm 安装后重启会找不到 bridge。

已经修复并验证：

- 发布 `@openbrt/weclawbotctl@0.1.9`。
  - 移除了未落地的 raw text 直发路径；CLI 只保留 `screen <doc.json>`、
    `thinking`、`idle` 等硬件边界明确的命令。
  - 插件新增 `weclawbot_status`、`weclawbot_publish_screen_document`、
    `weclawbot_publish_activity`，并在 manifest 声明 `contracts.tools`。
  - `systemd/weclawbot-openclaw-curator.service` 改为从
    `%h/.npm-global/bin/weclawbot-openclaw-bridge` 启动，并带
    `NODE_EXTRA_CA_CERTS`。
  - CLI 新增 `weclawbotctl openclaw install` / `weclawbotctl openclaw doctor`：
    检查 OpenClaw 版本、插件安装、`contracts.tools` 诊断和 gateway 可达性；
    要求 OpenClaw `>=2026.6.9`。非交互 SSH 或 systemd 环境下即使 PATH
    没有 npm global bin，也会探测同目录或常见位置的 `openclaw`。
- 100 服务器已经升级并启用 npm 版插件：
  - `openclaw plugins inspect weclawbot` -> `Version: 0.1.9`
  - `openclaw plugins doctor` -> 无 weclawbot 问题
  - gateway 重启后日志显示 `http server listening (3 plugins: browser, telegram, weclawbot)`
- 普适结论：
  - 普适部分是 npm 包、OpenClaw manifest 的 `contracts.tools`、插件工具、skill
    文案、`openclaw install/doctor` 和像素文档边界。
  - 100 专属部分是本机 OpenClaw gateway 自签 TLS 证书修复、SAN 重签、
    systemd drop-in 和 100 的 workspace `TOOLS.md` 追加说明；其他用户只有在
    本地 gateway 出现同类 TLS 报错时才需要按 doctor 提示处理。
- 100 服务器 gateway TLS 已生成带 SAN 的本地证书：
  `DNS:openclaw-gateway,DNS:localhost,IP:127.0.0.1,IP:192.168.8.100`，
  并通过 systemd drop-in 设置
  `NODE_EXTRA_CA_CERTS=/home/csc/.openclaw/gateway/tls/gateway-cert.pem`。
  重启后 Telegram polling 正常启动，不再出现 self-signed/ALTNAME 错误。
- 100 的 main workspace `TOOLS.md` 已补充 WeClawBot 物理屏规则：先查
  `/home/csc/.npm-global/bin/weclawbotctl doctor --online`，再生成像素文档并
  `screen` 发布；不要用 OpenClaw Canvas，不要给固件发 raw text。
- 非投递 smoke test 已通过：
  `openclaw agent --session-key agent:main:weclawbot-codex-smoke-20260627 ...`
  的 system prompt 注入了 `weclawbot-curator` skill 和
  `weclawbot_status` / `weclawbot_publish_screen_document` /
  `weclawbot_publish_activity` 工具；实际调用了 `weclawbot_status` 和
  `weclawbotctl doctor --online`，没有使用 Canvas。

仍要注意：

- 2026-06-27 已定位并修复 100 服务器上 TG bot / OpenClaw UI “回复已推上去
  但屏不更新”的问题：根因是远端脚本和 Agent 生成的文档使用空
  `base_revision`，真机已有非空 `screen_revision` 后被固件拒绝为
  `stale_screen_revision`；旧版 `weclawbotctl` 只确认 MQTT publish，不等待
  屏端 `applied/rejected` 回执，所以误报成功。
- 已发布 `@openbrt/weclawbotctl@0.1.9`，`screen` 默认等待设备 status topic：
  `applied` 才退出成功，`rejected` 或超时会失败；OpenClaw 插件工具
  `weclawbot_publish_screen_document` 同步返回 `applied/rejected/status`。
- 真机已刷入并验证 `0.1.76`：`version=0.1.76`、`agent_mode=byoa`、
  `agent_transport_state=online`、MQTT 在线；本版修复 BYOA 已配对日历页
  左下悬浮桌宠被“智能体状态”覆盖层关闭的问题。未配对绑定码仍放在
  日历左下；已配对后回到普通日历页并显示空闲桌宠。
- 真机已刷入并验证 `0.1.77`：BYOA `thinking` 记录 `correlation_id`，
  `idle` 必须匹配同一个 id 才能回落；串口状态和 device context 暴露当前
  activity id 与剩余秒数，避免旧任务/cron 误清新任务的思考状态。100
  服务器实测错误 `idle` 被拒绝为 `activity_correlation_mismatch`，正确
  `idle` 可回落。
- `0.1.75` 已验证 `force_replace` / `base_revision="*"` 的直接覆盖路径可用。
- 100 服务器已升级全局 CLI 与 OpenClaw 插件到 `0.1.9`，并重启
  `openclaw-gateway.service` 和 `weclawbot-openclaw-curator.service`；
  `weclawbotctl openclaw doctor --json` 显示 plugin `Version: 0.1.9`、
  gateway reachable。
- 100 上原 5 分钟状态卡 cron 是 system crontab，不是 OpenClaw cron。它之前
  一直在跑但只是 `published`，实际被屏端拒绝。现在脚本改为保存自己上一张
  状态卡的 revision，只有当前屏仍是上一张状态卡时才续写；如果用户/TG/UI
  已经换屏，脚本会因 `stale_screen_revision` 跳过并补发 `idle`，不再抢屏。
- 已跑通 OpenClaw agent E2E：新 session 生成 `368 x 206` mono1 文档并通过
  `/home/csc/.npm-global/bin/weclawbotctl screen --force --timeout 20` 上屏，
  agent 回复 `已上屏，doc-e2e-20260627102413`；串口确认后续手动内容
  `doc-hold-20260627102800` 未被 cron 覆盖。
- 普通 shell 里运行 OpenClaw CLI 访问本地 WSS gateway 时，需要带
  `NODE_EXTRA_CA_CERTS=/home/csc/.openclaw/gateway/tls/gateway-cert.pem`；
  systemd 下的 gateway 和 curator 已配置。
- 2026-06-28 确认真机当前内容 `current_note.page_count=1`，说明“没有分屏”
  是 Agent 只提交了一页像素文档，不是固件翻页失效。已把页数事实和硬件边界
  写入 OpenClaw/Hermes skill、workspace 指令、README 和协议文档：
  `pages.length` 就是真机页数，固件不会二次拆分。OpenClaw 包源码已升到
  `0.1.13`，并用 tarball 临时更新 100 服务器插件。
- 2026-06-28 在 100 上用新版 OpenClaw 插件跑版面回归：要求 8 条任务不要
  一页硬塞，agent 最终发布 `wknd-20260628063358`，真机串口回读
  `current_note.page_count=2`、`dashboard_view=note`、MQTT online。说明
  分页链路可用。`0.1.11` 起 publish 工具和 CLI 成功返回时也带
  `warnings`/`layout_guidance`，避免 agent 只看到 `applied=true` 而忽略
  页数事实和发布前自评。
- `0.1.12` 起明确修正产品原则：呈现效果评估留在 Agent 端，Agent 生成 bitmap
  后应自评可读性、拥挤度、分页和整体观感，必要时重新生成；固件不写死字体、
  换行、分页审美，避免用户为版面策略升级反复刷固件。
- `0.1.13` 进一步明确 skill 升级边界：WeClawBot 方主要描述硬件边界、协议和可观测
  反馈；用户和 Agent 在使用中沉淀的版式偏好、审美判断和工作流习惯必须保留，
  除非用户明确要求改变或触碰硬件边界。
- `0.1.14` 修复 BYOA Agent 把“清屏”误渲染成纯黑 `screen_document` 的事故：
  清屏必须通过 `screen_clear`，OpenClaw/Hermes 插件和 `weclawbotctl clear`
  均提供显式入口；validator 直接拒绝全页纯色的内容 `screen_document`，避免
  黑图/白图/空白图被当作清屏发布。OpenClaw bridge 也会在每个 curator job
  前后自动同步 `thinking` / `idle`，让最小思考状态不依赖模型自觉调用工具。
  `@openbrt/weclawbotctl@0.1.14` 已发布到 npm latest，并已更新 100 服务器。
- `0.1.15` 针对 OpenClaw Telegram/UI 直接任务补齐思考态：插件注册
  `before_agent_run` / `before_agent_finalize` / `agent_end` hook，对提到
  WeClawBot/屏幕/上屏的长程 turn 自动显示并回落桌宠思考态。安装器会启用
  `plugins.entries.weclawbot.hooks.allowConversationAccess=true`，doctor 会检查。
- `0.1.16` 修正上屏后用户看不到效果预览的问题：`weclawbotctl preview` 可单独
  渲染预览 PNG，`weclawbotctl screen` 和 `weclawbot_publish_screen_document`
  默认返回 `preview.pages[].path`。Agent 必须把这些 PNG 通过自己的 TG/UI
  消息渠道发给用户；插件自动回传不再作为主路径。
- `0.1.17` 把 OpenClaw 插件 `auto_preview` 改为默认关闭。效果预览由 Agent
  自己检查、自己通过当前聊天/媒体通道交付给用户，自动附件仅作为显式开启的兼容实验。
- `0.1.18` 明确效果图交付的终极目的不是“证明 MQTT 发出去了”，而是让用户看见真实版面并持续
  纠偏，帮助 Agent 越来越贴近用户的审美、阅读密度、字体偏好和分页习惯。
- `0.1.19` 尝试把“上屏后效果图回传”从 prompt 义务改成工具链契约：
  `weclawbotctl screen` 和 `weclawbot_publish_screen_document` 成功发布后写入
  preview manifest；OpenClaw hook 在当前 run 收尾时扫描 manifest，并通过
  当前 Telegram/UI 会话发送 PNG。这样 Agent 即使用 Python/脚本间接调用 CLI，
  也不应漏掉效果图。
- 100 服务器已安装并重启 OpenClaw gateway，doctor 显示
  `@openbrt/weclawbotctl@0.1.19` loaded。一次 TG 路径初测已通过：
  `weclawbotctl screen` 写出 manifest，hook fallback 到 Telegram outbound
  `sendPhoto` 成功，manifest 被标记 `delivered_at`。
- 2026-06-30 已将“上屏后无效果图”的多轮尝试和剩余问题整理为
  `docs/agent-preview-delivery-discussion.md`，便于后续和 OpenClaw / Agent
  同学讨论 lazy skill、workspace 强注入、通用 artifact 交付等方案。

已经实现：

- 固件端 `screen_document` 校验与显示
- 固件端 `activity` 思考状态
- `0.1.60` 起，固件端 `screen_document` 同时支持
  `target=idle_photo` / `target=photo`，允许 1 页 `400 x 300` fullscreen
  `mono1` 照片帧，接收后写入 idle photo 并立即显示
- 固件端 BYOA bootstrap、临时配对、永久凭证存储
- 固件端已具备统一 MQTT 控制通道；官方模式下微信文本、语音转写、反馈、
  图片/文件元数据会优先发布到 MQTT `events` topic，失败时才回落到
  legacy HTTP curator 路径；BYOA 配对后这些微信入口直接忽略
- 官方模式会通过内置 `/gateway` bootstrap 获取屏端 MQTT/WSS 凭证；
  设备端使用 `off_<安装设备 id>`，BYOA 继续使用 `wec_<安装设备 id>`，
  避免官方与自定义智能体 ACL 串台
- `0.1.54` 起，微信文本在 official 模式下不再静默 fallback 到
  legacy HTTP curator；若 MQTT 正在重连，固件会把最近 3 条微信文本暂存
  5 分钟，恢复后自动发布，不再要求用户重发
- `0.1.54` 修复了屏端 MQTT 状态机问题：销毁后的旧 esp-mqtt client 迟到
  事件不再能把 `Connected()` 标成 true；`Connected()` 同时要求 client 句柄存在
- `0.1.54` 将设备上行发布改为 QoS 0 直接写入，并关闭 mbedTLS 硬件 AES，
  避开 ESP32-S3 在低内部 DMA heap 时 `esp-aes` 写包失败和 QoS1 outbox 卡死
- 串口新增 `WEC:WECHAT_TEST <文本>`，用于在不依赖真实微信重发的情况下走
  同一个 `HandleText -> MQTT event -> official bridge` 路径
- 固件端支持官方 Agent 下发 `screen_intent`：一个控制消息可同时携带
  `screen_document` 和需要由屏端 iLink 发出的 WeChat reply；BYOA 下
  `wechat_reply` 会被拒绝或忽略
- 固件端反馈上行 `kind=wechat_feedback`：
  - `修改为...` 本地即时替换当前微笺，同时上报 correction gap
  - `/clear` / `/clear all` 本地即时清屏，同时上报 rapid_clear gap
  - `+` / `贴上` 触发显式 recapture，从最近一次 ignore 回捞原文
- `WEC:GET` 已暴露 `screen_revision`、最近 Agent status 和最近 reject detail
- `WEC:GET` 已暴露 `idle_photo` 明细和 heap / internal / DMA 最大连续块，
  便于定位 MQTT task 创建失败、DMA 不足和照片是否真正落屏
- `WEC:GET` 已暴露 `agent_mqtt_connected`、`agent_transport_state` 和
  `agent_device_id`，配置页可区分“已配对”和“MQTT 在线”
- `WEC:SET` 已支持 `agent_mode=official|byoa`，`curator_url` 仅保留为
  legacy 兼容字段
- 网关端 bootstrap / claim / MQTT credential provisioning
- 网关端官方 MQTT bootstrap 已加入：`weclawbot.gateway.v1/bootstrap`
  会发放官方屏端凭证，并启动官方 bridge 监听该设备 `events/status`
  topic；bridge 复用当前 curator 决策，再包成 MQTT `screen_intent` /
  `screen_clear` 控制消息返回屏端
- 网关端已支持服务重启后从 Redis 恢复 official MQTT bridge；否则已配对
  设备仍连着 broker，但 proxy 重启后无人订阅其 `events` topic
- 网关端增加 ignored direct-text promotion：若上游把一段明确可贴屏的
  微信文本误判为 `ignore`，proxy 会兜底生成 `create_note` 并继续渲染
  `screen_document`
- 网关端 feedback 事件不再进入模型上游；recapture 命中后直接返回可上屏 note
- `2026-06-25` 已修复图片上屏严重回归：官方 bridge 对
  `set_idle_photo` / `replace_idle_photo` 不再套用 content-note 过滤逻辑，
  会保留 `viewport=fullscreen` 页面并下发 `screen_document.target=idle_photo`；
  idle photo 不要求旧 `base_revision` 匹配
- Hermes 插件：绑定、状态、思考状态、屏幕文档工具
- OpenClaw 插件：绑定、activity、screen document CLI/skill
- 网关 WSS 端到端模拟测试通过

真机已知测试：

- 2026-06-25 已对真机重新刷入 `0.1.65` 固件。
  - 端口：`/dev/cu.usbmodem212201`
  - MAC：`44:1b:f6:95:3e:d4`
  - 本次 flash 未擦写 NVS，保留 Wi-Fi / 微信 / Agent 配置。
  - 刷写后确认：`version=0.1.65`、Wi-Fi `504-614`、
    IP `192.168.8.192`、微信 `已连接`、official device id
    `off_wec_68a9a7e622233f64`、`agent_transport_state=online`、
    `agent_mqtt_connected=true`。
  - 本次修复了照片页后“三屏轮播像被锁住”的回归。根因不是云端
    没下发，而是屏端任务/内存顺序不稳：
    - `0.1.60` 的独立 screensaver/carousel task 在真机低内部 RAM 下
      实际未稳定创建，照片页显示后没有轮播调度器。
    - `0.1.63` 虽把轮播改进主循环，但环境任务在 MQTT 抢占内部 RAM 后
      创建失败。
    - `0.1.64` 证明了“先预留环境任务栈、MQTT online 后再启动重绘”
      的方向正确，但 agent pump 栈压到 4KB 后真机报 stack overflow。
    - `0.1.65` 最终采用：环境任务先创建但在 MQTT online 前只做轻量状态
      更新；轮播计时改用 `esp_timer` 单调时间；TimeSync 合并进环境任务；
      official saved credentials 连接握手期间不立即二次 bootstrap；agent pump
      保持 6KB 栈；MQTT keepalive 调整为 60 秒。
  - 长稳验证：MQTT online 后连续约 95 秒保持在线，`drops=0`、
    `errors=[]`，串口抓到 9 次轮播跳转：
    `note -> calendar -> photo -> note -> calendar -> photo -> note -> calendar -> photo -> note`。
  - 发图后验证：`WEC:WECHAT_IMAGE_TEST` 在 MQTT online 后发布成功，
    `payload_bytes=2006`、`mqtt_publish_result=0`、`published=true`；
    官方 bridge 回写新 idle photo：
    `wxmedia_1782361777_9966026b_photo_mqt0xlxw`，
    `400 x 300`、`stride=50`。照片应用后继续轮播：
    `photo -> note -> calendar`，未再锁在照片屏。
- 2026-06-25 已将左右短按改为统一手动翻屏，随 `0.1.66` 刷入真机：
  - 右键顺序：`日历 -> 照片 -> 留言第 1 页 -> 留言第 2 页 -> 留言第 3 页 -> 日历`。
  - 左键按相反方向循环；没有照片或没有留言时自动跳过对应屏。
  - 留言手动翻页最多取 3 页，因此总屏数最多 5 屏。
  - 长按语义未改：左长按清文字留言，右长按全清并重新登录。
  - 刷写后确认：`version=0.1.66`、`agent_transport_state=online`、
    `agent_mqtt_connected=true`，自动轮播仍正常：
    `note -> calendar -> photo -> note`。
- 2026-06-25 已修复桌宠“思考中”不回落问题，随 `0.1.67` 刷入真机：
  - 根因一：云端只要求屏端发微信回复、但不下发 `screen_document` 时，
    固件发出 iLink 回复后没有重新渲染当前屏，可能一直停在 thinking。
  - 根因二：部分本地微信/图片/文件入口直接调用 `ShowThinking`，没有统一
    TTL 兜底。
  - 修复：reply-only `screen_intent`、独立 `wechat_reply` 成功/失败后都会
    回到当前留言/照片/日历；本地 thinking 统一走 `ShowThinkingWithTimeout`
    并设置默认 90 秒超时，云端 `activity=thinking` 仍使用下发 TTL。
  - 刷写后确认：`version=0.1.67`、Wi-Fi `504-614`、
    IP `192.168.8.192`、微信 `已连接`、`agent_transport_state=online`、
    `agent_mqtt_connected=true`。
  - 自动轮播验证：`note -> calendar -> photo -> note`，未锁在照片屏。
  - 回归验证：在 MQTT 尚未 online 时通过 `WEC:WECHAT_TEST` 模拟微信文本，
    设备先进入 thinking 并暂存，MQTT 恢复后发布事件，云端返回后落到
    `dashboard_view=note`、`agent_last_status_kind=applied`，未停留在
    `dashboard_view=other`。
- 2026-06-25 已修复中文“清屏/清空屏幕”无动作问题，随 `0.1.68` 刷入真机：
  - 根因：官方云端回复提示用户可发“清屏”，但固件本地命令只认
    `/clear` / `/clear all`；用户发“清空屏幕”后事件进入云端模型，
    被判为 ignore，于是只收到“这条没有贴到屏幕”的说明，屏端没有清空。
  - 固件修复：`清屏`、`清空屏幕`、`清除屏幕`、`清空微笺`、
    `清空留言` 等中文命令映射到清当前微笺；`全清`、`清空全部`、
    `清空所有留言` 等映射到清空全部微笺；`清空照片` 仍只清照片屏。
  - 服务端兜底：official bridge 本地规则也识别中文清屏并生成
    `screen_clear`，避免旧固件或异常路径再把清屏交给模型；本地代码已改，
    VPS 部署待确认 SSH 主机指纹后执行（本次连接返回
    `Host key verification failed`，未绕过校验）。
  - 刷写后确认：`version=0.1.68`、Wi-Fi `504-614`、
    IP `192.168.8.192`、微信 `已连接`、`agent_transport_state=online`、
    `agent_mqtt_connected=true`。
  - 回归验证：清屏前 `note_count=1`；串口模拟
    `WEC:WECHAT_TEST 清空屏幕` 后命中
    `scope=command stage=clear_current`，最终 `note_count=0`、
    `dashboard_view=photo`，未再进入云端 ignore 文案。
- 2026-06-25 推进 `0.1.69` BYOA 配对和插件配置体验：
  - BYOA 屏端不再要求用户填写 URL 或扫描微信二维码，而是在日历页显示
    六位 `AI智能体绑定码` 和有效期倒计时；Agent 端输入配对码完成绑定。
  - Hermes / OpenClaw 统一采用本地 MQTT profile：`bind` 保存设备级凭据、
    `status` 查看状态、`doctor --online` 做连通性诊断、`export` 显式导出给
    其他本地 Agent 或 MQTT 工具，默认不打印密码。
  - 本机已发现可纳入接入测试的 coding agent：Codex、Claude Code、
    Gemini CLI、OpenCode、OpenClaw、Hermes；优先通过通用 `weclawbotctl`
    命令复用同一套 MQTT 凭据，避免为每个 Agent 重做协议。
  - VPS `weclawbot-curator-proxy.service` 已部署 BYOA 旧绑定自愈：当设备
    本地 BYOA 凭据丢失但 Redis 仍有旧 binding 时，新的 bootstrap 会先
    revoke 旧 device/agent ACL，再生成新的配对码，避免 `409
    device_already_paired` 卡死。
  - 真机已刷入并验证 `0.1.69`：
    `version=0.1.69`、Wi-Fi `504-614`、IP `192.168.8.192`。
  - BYOA 真机 E2E 已跑通：屏端显示配对码 -> `weclawbotctl bind` ->
    Agent `doctor --online` -> 屏端 `agent_transport_state=online` ->
    `thinking`/`idle` -> `screen_document` applied。
  - 测试结束后已恢复 official 模式并清掉测试 note：
    `agent_mqtt_connected=true`、微信 `已连接`、`note_count=0`、
    `dashboard_view=photo`；本机 `/tmp` 测试凭据已删除，E2E 产生的
    BYOA binding 已通过 bootstrap repair revoke。
- 2026-06-25 已刷入并验证 `0.1.71` 配置页和 BYOA 配对体验修正：
  - 配置页将“保存”和“重启”合并为 `保存并重启`，危险操作折叠到
    `高级操作`，串口输出默认折叠，降低普通用户配置复杂度。
  - 已保存 Wi-Fi 密码不会被读回明文；配置页显示“已保存，留空不修改”，
    SSID 未变且密码为空时只发送 `agent_mode`，不会覆盖 NVS 里的密码。
  - 新增串口命令 `WEC:CLEAR_AGENT`，配置页 `解绑智能体` 会清除 BYOA
    凭据，便于重新显示配对码。
  - BYOA 配对码从独立页面改回日历主屏左下角醒目显示；普通官方路径仍是
    微信二维码，BYOA 路径是 Agent 端输入六位配对码。
  - 通用 Agent CLI 已发布到 npm：`@openbrt/weclawbotctl`。最初验证版本为
    `0.1.3`，当前 npm latest 为 `0.1.9`；提供跨平台 `weclawbotctl`，
    同时保留 OpenClaw 插件元数据；已补齐 OpenClaw `contracts.tools`、
    插件安装/诊断子命令和非交互 PATH 探测。`npm pack --dry-run` 确认包体
    不包含 `node_modules` 或本地凭据。
    早期误发的 `weclawbot-openclaw-plugin`、
    `@ccqqbb_cb/weclawbot-openclaw-plugin` 和无 scope `weclawbotctl`
    已 deprecated 到 `@openbrt/weclawbotctl`；彻底 unpublish 需要 npm
    2FA OTP。
  - npm README 第一屏已改成面向 Agent 的安装说明：用户只需给 Agent
    一句“Install @openbrt/weclawbotctl and connect ... with pairing code
    123456”，Agent 应自行执行 install、bind、doctor，并知道后续
    `thinking` / `idle` / `screen` 的用法。
  - 真机刷写确认：`version=0.1.71`、Wi-Fi `504-614`、
    IP `192.168.8.192`、`agent_mode=byoa`、`agent_paired=false`、
    `agent_transport_state=awaiting_pairing`、`dashboard_view=calendar`。
- 2026-06-27 已修复命令行 BYOA 配对后照片页锁住的问题，随 `0.1.73`
  刷入真机：
  - 现象：用户在远端 Agent 侧用 `weclawbotctl bind` 配对后，真机固定在
    照片屏；左右按键可手动翻屏，但自动轮播不再运行。
  - 根因：自动轮播本身不应依赖微信在线，但外层 `EnvironmentTask` 仍要求
    `bot.Connected()` 和 `AgentMqttConnected()` 同时为 true 才调用
    `ProcessDashboardTimers()`；BYOA 场景下 Agent MQTT 已在线，微信通道
    可能未连接，导致计时器根本没有执行。
  - 修复：屏幕运行时改为“微信通道或 Agent MQTT 任一在线即可运行”；
    自动轮播条件只要求 Agent MQTT ready、当前是 dashboard view、且有留言
    或 idle photo 内容。
  - 已验证中间版 `0.1.72` 仍会在 47 秒内一直停留 `dashboard_view=photo`，
    证明真正卡点在外层任务门槛。
  - 真机刷写确认：`version=0.1.73`、`agent_mode=byoa`、
    `agent_paired=true`、`agent_mqtt_connected=true`、
    `agent_transport_state=online`、`note_count=0`、
    `idle_photo_configured=true`。
  - 自动轮播验证：50 秒状态序列出现
    `photo -> calendar -> photo -> calendar -> photo`；进一步解析串口事件
    抓到 `carousel_advance calendar -> photo` 和
    `carousel_advance photo -> calendar`，确认无需微信在线也会继续轮播。
- 2026-06-27 已收紧 BYOA 所有权边界，随 `0.1.74` 刷入真机：
  - 产品语义：BYOA 是用户自带智能体接管屏幕，微信/iLink 不再作为入口、
    回复通道或控制依赖。
  - 固件修复：BYOA 启动时显式将微信连接语义置为未启用，清空旧微信事件
    待发送队列；`WECHAT_TEST` / `WECHAT_IMAGE_TEST` 在 BYOA 下返回 ignored；
    真实 `getupdates` item 即使误入也直接 `ingress_ignored`，不触发 thinking、
    不暂存、不上报 MQTT、不回微信。
  - Agent 控制修复：BYOA 下 `screen_document` / `activity` / `screen_clear`
    仍有效；`wechat_reply` 控制会被拒绝，`screen_intent` 里的
    `wechat_reply` 会被忽略，底层 `SendTextMessage` / 预览图发送也加了
    BYOA 保护。
  - `device_context.wechat_transport` 在 BYOA 下广告为
    `mode=disabled`、`direction=ignored`、`reason=byoa_agent_owns_screen`。
  - 真机刷写确认：`version=0.1.74`、`agent_mode=byoa`、
    `custom_agent_mode=true`、`agent_paired=true`、
    `agent_mqtt_connected=true`、`agent_transport_state=online`、
    `wechat_connected=false`、`dashboard_view=calendar`。
  - BYOA 微信入口验证：串口 `WEC:WECHAT_TEST ...` 返回
    `type=wechat_test_ignored` / `message=BYOA 模式忽略微信入口`；
    之后 `screensaver_active=false`、`note_count=0`、
    `agent_last_status_kind=online`，未进入 thinking，未上屏，未转发。
  - 自动轮播验证：在 `wechat_connected=false`、`agent_mqtt_connected=true`
    下抓到 `photo -> calendar -> photo -> calendar`，事件包含
    `carousel_advance photo -> calendar` 和 `calendar -> photo`。
- 2026-06-25 已对真机重新刷入 `0.1.60` 固件。
  - 端口：`/dev/cu.usbmodem212201`
  - MAC：`44:1b:f6:95:3e:d4`
  - 本次 flash 未擦写 NVS，保留 Wi-Fi / 微信 / Agent 配置。
  - 刷写后确认：`version=0.1.60`、Wi-Fi `504-614`、
    IP `192.168.8.192`、微信 `已连接`、official device id
    `off_wec_68a9a7e622233f64`、`agent_transport_state=online`、
    `agent_mqtt_connected=true`。
  - 本次修复前，`0.1.59` 在真机上出现 `mqtt_client:
    Error create mqtt task`；抓到的启动前后 heap 为 internal
    free `14175`、largest `7680`，esp-mqtt init 后 largest 降到 `3584`，
    无法再创建 `4096` 栈的 `mqtt_task`。
  - `0.1.60` 将 ESP-MQTT in/out buffer 从 `2048` 降到 `1024`，保留
    `mqtt_task` 4KB 栈；刷机后 MQTT 已 online，未再出现
    `Error create mqtt task`。一次稳定状态下 `WEC:GET` 读到
    internal largest `4864`、DMA largest `4864`。
- 2026-06-25 已跑通真机图片到官方云端再回 idle photo 的 E2E：
  - 串口触发：`WEC:WECHAT_IMAGE_TEST
    https://images.unsplash.com/photo-1500530855697-b586d89ba3ee?w=400&h=300&fit=crop`
  - 屏端日志：`wechat_attachment_publish`，`mqtt_connected=true`、
    `payload_bytes=2006`、`mqtt_publish_result=0`、`published=true`
  - VPS 官方 bridge 日志：
    `official_mqtt_event kind=wechat_image` ->
    `attachment_extract ok=true reason=photo_frame format=.jpg` ->
    `decision action=set_idle_photo` ->
    `official_mqtt_control_published kind=screen_intent action=set_idle_photo`
  - 真机最终状态：`agent_last_status_kind=applied`，
    `agent_last_status_detail=wxmedia_1782359228_96ca0169_photo_mqszeylk`，
    `idle_photo.revision=wxmedia_1782359228_96ca0169_photo_mqszeylk`，
    `idle_photo.width=400`、`height=300`、`stride=50`。
- 本次图片事故根因：
  - 图片事件实际上已经到达云端，且云端可在约 1-2 秒内提取成
    `photo_frame`。
  - 旧 bridge 只把 content-note 类动作生成 `screen_document`，并过滤掉
    fullscreen 页面；`set_idle_photo` 只剩微信回复，没有给屏端文档。
  - 旧固件只接受 `target=content` 和 `368 x 206` 内容区尺寸，即使云端下发
    fullscreen idle photo 也会被拒绝。
  - `0.1.60` 同时修了云端 document 生成、固件 idle photo 接收显示，以及
    reply-only intent 不再让桌宠一直处于 thinking。
- 2026-06-25 已对真机重新刷入 `0.1.55` 固件。
  - 端口：`/dev/cu.usbmodem212201`
  - MAC：`44:1b:f6:95:3e:d4`
  - 本次 flash 未擦写 NVS，保留 Wi-Fi / Agent 配置。
- 刷写后已通过串口把 `agent_mode` 从 BYOA 切到 official，并确认：
  - Wi-Fi：`504-614`
  - IP：`192.168.8.192`
  - official device id：`off_wec_68a9a7e622233f64`
  - `agent_transport_state=online`
  - `agent_mqtt_connected=true`
- 2026-06-25 00:49 CST，真机官方智能体 E2E 已用
  `WEC:WECHAT_TEST 0.1.55 E2E：床前明月光，疑似地上霜。` 跑通：
  - 屏端 WEC 日志：`mqtt_publish_result=0`、`mqtt_publish_qos=0`、
    `published=true`
  - VPS 官方 bridge 收到 `official_mqtt_event kind=wechat_text`
  - Hermes/curator 返回 `create_note`，网关 direct-text promotion 兜底生效
  - bridge 发布 `official_mqtt_control_published kind=screen_intent`
  - 真机应用成功并回 `agent_last_status_kind=applied`
  - 应用后的 revision/render id：`wxmqtt_1782318639_a3c7812a_mqsb6bzq`
  - 真机最终状态：`agent_transport_state=online`、
    `screen_revision=wxmqtt_1782318639_a3c7812a_mqsb6bzq`
- 历史记录：2026-06-24 的 `0.1.48` 官方 MQTT 注入 E2E 曾跑通：
  - VPS 官方 bridge 收到 `wechat_text` event
  - curator 返回 `create_note`
  - bridge 发布 `screen_intent` + `screen_document`
  - 真机应用成功并回 `device_status kind=applied`
  - 应用后的便签 revision/render id：`f0a0b00b_mqs0feuw`
  - 真机最终状态：`note_count=1`，`screen_revision=f0a0b00b_mqs0feuw`
  - 收尾复查：`agent_transport_state=online`，`agent_mqtt_connected=true`
- 测试过程中临时 MQTT dynsec client 创建/删除后，真机曾短暂进入
  `agent_transport_state=reconnecting`，约 1 分钟后自动恢复 online；
  后续建议给 esp-mqtt error/reconnect 原因增加更细日志。
- 当前微信 iLink 状态为 `已连接`。`0.1.55` 已用 `WEC:WECHAT_TEST`
  覆盖固件收到文本后的 `HandleText -> MQTT event -> official bridge`
  主路径；尚未让用户在微信里真实重发一次来覆盖 `getupdates` 入口。
- 2026-06-24 23:33 真实微信已进入设备：用户发送“床前明月光，疑似地上霜。”
  后，旧固件在 MQTT 短暂 reconnect 时 fallback 到 legacy HTTP，且上游判
  `action=ignore`，所以只回微信没有上屏。`0.1.54` 起已修正屏端重连窗口和
  MQTT 上行问题，`0.1.55` 已复刷真机并验证官方 E2E；真实微信重新发送后的
  `getupdates` 入口仍建议再复测一次。
  当前收尾状态为 `wechat_state=已连接`、`agent_transport_state=online`。
- 自定义智能体模式曾生成绑定码并被 Hermes 绑定成功，Hermes 本地凭证保存
  到了用户配置目录。
- 串口 DTR=false 打开可能触发设备重启；验证时使用 DTR=true 读取 `WEC:GET`
  更稳定。

云端已跑通：

- 2026-06-24 已部署官方 MQTT bridge 到 `weclawbot.link` VPS。
- Mosquitto WebSocket listener 已在本机 `127.0.0.1:9001` 启动；
  Caddy 通过公网 443 的 `wss://weclawbot.link/mqtt` 反代，不需要公开
  1883 / 8883。
- 2026-06-25 已重新部署 curator proxy / official bridge 图片修复版。
  服务重启后从 Redis 恢复 official bridge，并已订阅
  `off_wec_68a9a7e622233f64` 的 events/status topic。
- 官方 bridge 已支持图片决策的 fullscreen 回屏：
  `set_idle_photo` / `replace_idle_photo` -> `screen_intent` ->
  `screen_document.target=idle_photo` -> 1 页 `400 x 300` `mono1`。
- 官方 `/gateway` bootstrap E2E 已通过：
  模拟屏端获取官方 MQTT 凭证 -> 连接 WSS -> 订阅 control ->
  发布 `wechat_text` 到 events -> 官方 bridge 调 curator ->
  返回 `screen_intent` + `screen_document` -> 模拟屏端发布 applied status。
- 本次 E2E 返回 1 页 `mono1` content bitmap，尺寸 `368 x 206`、
  stride `46`，payload bytes `9476`。

本地已跑通：

- 使用干净 Homebrew Python 3.10 IDF venv + `ninja -C build` 成功生成
  `build/weclawbot.bin`。
- `scripts/prepare_web_firmware.sh` 已刷新 `web/firmware/` 分段刷写产物。
- 本地配置页已启动并验证 `http://localhost:8765/`、`manifest.json` 可访问。

后续建议：

- 已用 `WEC:WECHAT_IMAGE_TEST` 跑通真机图片 MQTT E2E；仍建议用户手机真实
  再发一张图片，补齐 iLink `getupdates` 入口的人工复测：
  iLink `getupdates` -> firmware -> MQTT event -> gateway/official bridge ->
  MQTT `screen_intent`/`activity` -> firmware。
- MQTT internal heap 余量仍偏紧，后续应继续削减常驻任务栈或把可外置的大
  buffer 移到 PSRAM，避免后续新功能再次挤爆 esp-mqtt。
- `WEC:GET` 已增加当前 `screen_revision`，方便 Agent 调试 stale revision
- 右键“全清”需要确认是否同时清除 Agent 绑定凭证
- 用户可见的“解绑智能体”入口已补；服务端 bootstrap 已能自愈“服务器已绑定
  但设备 NVS 丢失”的卡死，后续可继续补显式 revoke 当前 BYOA 绑定
- BYOA 下可选微信入口只是后续方向，当前屏端只应呈现配对码/已接管状态

## Hermes / OpenClaw 插件

仓库内已经有两个集成目录：

- `integrations/hermes/weclawbot-plugin`
- `integrations/openclaw/weclawbot-openclaw-plugin`

插件的核心职责不是替固件写死审美，而是把硬件边界告诉 Agent：

- 屏幕 400 x 300
- 内容区 368 x 206
- mono1
- 最多 3 页
- 自动翻页
- 慢刷新
- 页眉页脚由固件拥有
- Agent 只能发送 bounded screen document
- Agent 可发送短暂 thinking activity

用户自由度验证目标：

- 用户可以让自己的 Agent 定时把 token 消耗、任务状态、提醒、照片或
  任意自定义卡片发到屏上。
- BYOA Agent 不需要微信作为入口；BYOA 下微信/iLink 被忽略。官方模式下，
  微信事件进入 WeClawBot 官方 Agent，再由同一 MQTT 控制面上屏。
- “思考的桌宠”可以成为电脑/服务器旁的 Agent 工作状态副屏。

## 官网与仿真机

官网主题：

> 微信与 Agent 都能接管的桌面屏

官网方向：

- `weclawbot.link` 是项目官方网站和仿真机交互体验网站，也是用户忘记
  GitHub、忘记配置方式后仍能回来的入口。
- GitHub About / Homepage 必须指向 `https://weclawbot.link/`，不要改成
  GitHub Pages 安装页。
- `https://openbrt.github.io/weclawbot/web/` 是公开固件安装与设备配置入口，
  应在官网、README、GitHub Pages 项目页中明确可点击，但不替代项目官网。
- 官网里需要有真实交互的 3D 仿真机，而不是普通产品截图。
- 用户可以在网页里扫码、发送消息、看屏幕变化，理解官方模式真机行为；
  BYOA/自定义智能体通过本地配置页和配对码完成。
- 仿真机应通过公开仓库的 firmware contract 同步行为，而不是手写一套。

当前状态：

- 本机/私有工作区已有 `site-experience/` 官网仿真项目
- 仿真机背景改为儿童书桌场景
- 3D 模型由另一路同学继续优化
- 公开仓库文档已经定义了官网同步机制
- 官网部署应由私有仓库流水线完成，公开仓库只发布同步机制和
  `web/firmware-contract.json`

已知问题：

- 官网仿真二维码尺寸、位置、倒计时和状态仍需与真机对齐
- 官网仿真里的电池/插电状态需要模拟成“未接线时显示电池/满电”
- 仿真交互与固件 contract 的自动同步还需要持续校验
- 高德 IP 定位天气已经作为仿真温湿度来源方向，但不能混淆为真机室内传感器

## 显示体验问题

已经明确的产品取向：

- 默认不应被留言页永久锁死
- 日历页、照片页、留言页可以轮流呈现
- 清空留言不等于清空照片
- 没有留言时，照片屏可以成为黑白相框
- 有微信登录态时才出现悬浮桌宠
- 未连接微信/Agent 前，日历页是兜底页
- 二维码可以和日历页共存，不必单独占满一屏

未完成或待确认：

- 三屏轮播规则：何时切页、何时暂停、何时回到留言页
- 照片和留言同时存在时的优先级
- BYOA 模式配对码、已接管状态和短暂 thinking 桌宠的共存规则
- 顶部状态栏图标完整规范：Wi-Fi、BLE、插电、电池、充电
- 中键长按开关机是否已经在当前固件完整实现

## 发布与公开仓库

Canonical 发布边界见
[public-release-boundary.md](public-release-boundary.md)。后续任何 agent 或
同学在 `openbrt/weclawbot` 上 staging、commit、push、tag 或 release 前都
应先按该文档检查，不再依赖临场口头提醒。
实际发布执行清单见
[project-release-checklist.md](project-release-checklist.md)：安装/配置页、
manifest、release 资产、Pages、固件二进制、项目文案和真机烟测都必须按
适用项核对。

公开仓库只应包含：

- 固件源码
- Web Serial 安装/配置页
- 固件 release 产物
- 用户文档
- Agent 插件源码和 skill 文档
- firmware contract
- 不含私密资源的示例图片

不应公开：

- 微信 token、iLink 登录态
- Wi-Fi 密码
- DeepSeek / 模型 API key
- MQTT broker 管理密钥
- 腾讯云、COS、SCF 私有资源 ID
- 官方云端提示词、用户日志、私有部署脚本

发布前必须处理：

- 统一版本号
- 生成固件 release，而不是只更新源码
- README 官网链接指向正式入口
- GitHub About 链接指向 `https://weclawbot.link/`
- GitHub Pages 安装/配置页作为 README 和官网里的明确入口，不作为 About
  homepage
- 示例真机图不能像贴纸，应使用真实场景或真机屏幕
- 官网体验链接与安装/配置页入口要清晰

## 常用本地命令

构建固件：

```sh
./scripts/idf.sh build
```

准备 Web Serial 固件：

```sh
./scripts/prepare_web_firmware.sh
```

启动本地配置页：

```sh
python3 scripts/serve_web.py
```

串口配置入口：

```text
WEC:HELLO
WEC:GET
WEC:SET {"ssid":"...", "password":"..."}
WEC:SET {"agent_mode":"official"}
WEC:SET {"agent_mode":"byoa"}
WEC:REBOOT
```

`curator_url` 仍存在于当前代码和历史串口协议中，但下一版用户配置面已改为
`agent_mode`，由固件映射到内置官方/BYOA 服务入口。

Hermes 插件常用命令：

```sh
hermes weclawbot bind <六位绑定码>
hermes weclawbot status
hermes weclawbot doctor --online
hermes weclawbot export --format env
hermes weclawbot screen <document.json>
```

OpenClaw / 其他本地 coding agent 常用命令：

```sh
weclawbotctl bind <六位绑定码>
weclawbotctl status
weclawbotctl doctor --online
weclawbotctl export --format env
weclawbotctl screen <document.json>
weclawbotctl clear
```

## 下一步建议

优先级从高到低：

1. 生成并核对当前开发版和公开版 release 产物。
2. 跑通线上 MQTT/WSS broker、Caddy 反代、provisioner、ACL 和 pairing。
3. 真机验证官方 MQTT bridge 的 Redis/provisioner/broker 长连接恢复行为。
4. 线上部署并真机验证 `kind=wechat_feedback`：correction、rapid_clear、recapture。
5. 明确三屏轮播和清空照片/留言/绑定态的状态机。
6. 让官网仿真机从公开 firmware contract 自动更新行为。
7. 用 Hermes/OpenClaw 各做一个真实示例：定时 token 消耗卡片上屏。
