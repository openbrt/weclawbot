# Agent 效果图交付问题讨论稿

日期：2026-06-30

本文记录 BYOA / OpenClaw Agent 在“上屏成功后没有把效果图发回用户”问题上的多轮尝试、现场证据和待讨论方向。目标不是把责任推给某一层，而是把链路拆清楚，方便其他同学一起判断应该在哪一层收口。

## 一句话问题

用户说“把内容放我屏上”后，真机屏幕已经更新，但 Telegram / OpenClaw UI 里只收到“已发到屏上”的文字，没有收到同一份屏幕效果图。该现象已在 Telegram bot 和 OpenClaw 管理 UI 两条入口出现，因此不是 Telegram 单一通道的发送接口问题，而是更上层的 Agent artifact 交付契约没有生效。

这会破坏一个关键产品目标：效果图不是单纯证明 MQTT 发出去了，而是让用户看见真实版面并持续纠偏，让 Agent 越来越贴近用户的审美、阅读密度、字体偏好和分页习惯。

## 期望行为

“上屏”应该是双交付：

1. 屏端收到 `screen_document` 并返回 `applied`。
2. 当前用户会话收到同一份屏幕效果图 PNG，作为可见回执和反馈入口。

如果第 2 步失败，Agent 不应该静默降级成“已发到屏上”。更合理的是明确说：

> 已上屏，但效果图发送失败：具体原因。

## 关键原则

- 固件只消费像素和声明硬件边界，不负责版面、字体、分屏、审美和聊天侧预览。
- Agent 负责内容选择、版式生成、预览评估、用户可见效果图交付和偏好学习。
- WeClawBot 插件可以提供预览产物和协议约束，但不应该私自接管 Telegram / UI 的通用发图能力。
- 用户不应该每次额外说“给我效果图”。在这个产品里，“上屏”本身就包含“给用户看最终效果”的义务。

## 已尝试的版本与结果

### 0.1.16：提供预览产物

改动：

- 新增 `weclawbotctl preview <document.json>`。
- `weclawbotctl screen` 默认返回 `preview.pages[].path`。
- `weclawbot_publish_screen_document` 返回 `preview.pages[].path`。

结果：

- CLI / tool 层具备了生成精确 mono1 预览 PNG 的能力。
- 但 Agent 仍可能不用该工具，或者生成了预览后不发给用户。

### 0.1.17：关闭默认自动预览钩子

改动：

- `auto_preview` 改为默认关闭。
- 预览图交付明确由 Agent 自己通过当前聊天/媒体通道完成。

结果：

- 符合“不要隐藏兜底，不要插件私自发 TG 图片”的方向。
- 但暴露出 Agent 流程没有硬约束：它可能只完成设备上屏，然后发文字状态。

### 0.1.18：强化 skill 文案

改动：

- 在 `weclawbot-curator/SKILL.md`、包内 `workspace/AGENTS.md`、README、协议文档中写入：
  - 效果图是 feedback surface。
  - 不只是 proof of delivery。
  - 长期目的是学习用户审美和阅读习惯。
  - 不应把“上屏”视为只更新真机。

结果：

- 100 服务器确认安装包中已有这些文案。
- 但实测仍然没有效果图。

### 0.1.19：preview manifest 确定性投递实验

改动：

- `weclawbotctl screen` 和 `weclawbot_publish_screen_document` 成功发布后写入
  `weclawbot.preview_manifest.v1`。
- PNG 默认落在 OpenClaw 可读的 `media/outbound/weclawbot-preview` 目录；
  manifest 记录 document id、页数、PNG 路径、发布时间和设备应用状态。
- OpenClaw hook 在当前 run 收尾时扫描 run 时间窗内未投递的 manifest，并通过
  当前 Telegram/UI delivery route 发送 PNG。

待验证：

- TG bot 当前 turn 是否收到 PNG 效果图。
- OpenClaw UI 当前会话是否收到 PNG 效果图。
- Agent 自写 Python/脚本间接调用 `weclawbotctl screen` 时是否同样生效。
- `before_agent_finalize` / `agent_end` 双 hook 是否不会重复投递。

初测结果（2026-06-30 01:23 CST，在 100 服务器）：

- 通过 `openclaw agent --session-key agent:main:main --deliver --reply-channel telegram`
  发起一次“生成测试卡并调用 `weclawbotctl screen`”任务。
- 真机发布路径成功，CLI 写入 manifest：
  `~/.openclaw/media/outbound/weclawbot-preview/manifests/...json`。
- manifest 被 hook 标记：
  - `delivered_at=2026-06-29T17:23:53.590Z`
  - `delivered_session_key=agent:main:main`
  - `delivered_channel=telegram`
- gateway 日志显示：
  - `session attachment unavailable: session attachments are restricted to bundled plugins`
  - fallback 到 outbound adapter 后 `sendPhoto` 成功，Telegram `messageId=107`
  - 随后才发送 agent final text，Telegram `messageId=108`
- 结论：TG 路径的 deterministic preview delivery 初测通过。
- 仍未验证：OpenClaw Web UI/dashboard 会话的媒体回传体验。

## 2026-06-29 实测证据

### 第一次实测：路径不被图片工具允许

用户请求：

> 把下来的世界杯赛程做个表格放我屏上

日志证据：

- OpenClaw 日志显示 Agent 生成了预览图：
  - `/home/csc/.cache/weclawbot/wc-schedule-preview.png`
- 但图片检查工具失败：
  - `Local media path is not under an allowed directory`
- 最后 Agent 只回复文字“已发到屏上”，没有发送媒体。

分析：

- 这次不是没有预览能力，而是预览放到了 OpenClaw 媒体工具不允许读取/发送的目录。
- Agent 没有恢复处理，也没有把图片移到可发送目录。

### 第二次实测：生成了预览，但没有调用发图工具

用户请求：

> 把今天的世界杯赛程做个表格放我屏上

真机结果：

- 屏幕已更新。
- Telegram 仍只收到文字，没有效果图。

现场文件：

- `/tmp/weclawbot-preview-bUFr7h/doc-20260629113328-wctoday-p1.png`
- `/home/csc/.cache/weclawbot/wc-today-preview.png`

会话轨迹：

```json
{
  "assistantTexts": [
    "已发到屏上 ✅\n\n**今日6/29赛程：**\n..."
  ],
  "toolMetas": [
    { "toolName": "write", "meta": "to ~/.openclaw/workspace/scripts/worldcup-today-screen" },
    { "toolName": "exec", "meta": "run python3 scripts/worldcup-today-screen (agent)" }
  ],
  "didSendViaMessagingTool": false,
  "messagingToolSentMediaUrls": []
}
```

分析：

- 这次连“路径被拒绝”的错误都没有出现。
- Agent 生成了预览，但把预览当成内部副产物，没有触发通用发图流程。
- 它通过 `write` + `exec` 自写脚本完成上屏，没有使用 `weclawbot_publish_screen_document` 这条可返回 `preview.pages[].path` 的标准工具路径。

### OpenClaw UI 入口同样没有效果图

用户补充：

> 在 openclaw UI 也没有上屏同时发出预览图

分析：

- 问题不应收窄为 Telegram adapter 的 `sendMedia` 或图片权限问题。
- 更像是 Agent 对“上屏”的任务完成定义仍停在“设备侧 applied + 文本汇报”，没有把“当前用户交互界面收到效果图”作为跨通道完成条件。
- 因此修复点应优先考虑 Agent prompt / workspace 强注入 / 通用 artifact delivery contract，而不是只修 TG 发图接口。

## 更深一层的发现

OpenClaw 当前 skill 机制是 lazy loading：

- 系统提示只列出 skill 名称、描述、路径和 hash。
- 模型需要在认为任务匹配时主动 `read` 对应 `SKILL.md`。
- 2026-06-29 的实测里，模型没有读取 `weclawbot-curator/SKILL.md`。

所以：

- `0.1.18` 的新规则确实安装到了 100 的 npm 包。
- 但它没有进入本次 Agent 的实际决策上下文。
- 真正每轮稳定注入的是 `/home/csc/.openclaw/workspace/AGENTS.md`、`TOOLS.md` 等 workspace 文件。

100 上当前 live workspace 里只看到旧提示：

> Use `/home/csc/.openclaw/workspace/scripts/weclawbot-status-screen` as a known-good example for Python/PIL rendering and MQTT publish. The OpenClaw plugin also exposes `weclawbot_status`, `weclawbot_publish_screen_document`, and `weclawbot_publish_activity`; prefer those tools when they are available in the session.

这条提示不够硬，甚至会继续鼓励 Agent 走“自己写 Python/PIL 脚本 -> CLI 上屏 -> 文字汇报”的旧路径。

## 目前判断

仅修改 npm 包内的 `SKILL.md` 不足以解决这个问题，因为关键规则不一定会进入模型上下文。

问题至少包含三层：

1. 语义层：用户说“上屏”时，模型把终点理解为物理屏，而不是“物理屏 + 当前会话效果图”。
2. 工具层：Agent 绕过标准 publish tool，自写脚本调用 CLI，导致预览路径和发图流程不可控。
3. 注入层：WeClawBot skill 是 lazy read，不是每轮强注入；真正强注入的 workspace `AGENTS.md` / `TOOLS.md` 没有这条完成条件。

## 候选方案

### 方案 A：live workspace 热修

在 100 的 `/home/csc/.openclaw/workspace/AGENTS.md` 或 `TOOLS.md` 中加入硬规则：

- 当用户请求“上屏 / 发到屏上 / 放我屏上 / 推到屏幕”时，完成条件是：
  - 设备端返回 `applied`；
  - 当前聊天/媒体通道收到最终效果图；
  - 如果效果图发送失败，必须明确报告原因。
- 优先使用 `weclawbot_publish_screen_document`。
- 如果写脚本，预览图必须输出到 OpenClaw 可作为媒体发送的目录。

优点：

- 最可能立刻影响 100 的 TG / UI 实测。
- 不依赖模型主动 read skill。

缺点：

- 是本地 workspace 规则，不是通用分发能力。
- 其他用户安装 npm 包后未必自动获得。

### 方案 B：安装器同步 workspace 规则

让 `weclawbotctl openclaw install` 在安装插件时，写入或合并一段短的 workspace 规则到 OpenClaw 的稳定注入文件。

优点：

- 其他用户安装后也能获得同样行为约束。
- 不再单纯依赖 lazy skill。

风险：

- 修改用户 workspace 文件需要非常谨慎。
- 需要可识别、可更新、可卸载的 managed block，不能覆盖用户和 Agent 已沉淀的本地偏好。

### 方案 C：把“上屏 + 预览交付”做成单一高阶工具

提供一个更高阶的工具，例如：

- `weclawbot_publish_and_prepare_preview`
- 或增强 `weclawbot_publish_screen_document` 的返回协议，让结果更像“必须交付给用户的附件清单”。

优点：

- 减少 Agent 自己写脚本、自己猜路径、自己处理预览的空间。
- 对 Hermes / OpenClaw / Codex 等 Agent 都更统一。

缺点：

- 仍然需要 Agent 调用正确工具。
- “发送媒体”本身仍是通用 Agent runtime 能力，不应做成 WeClawBot 私有 TG 发送器。

### 方案 D：恢复可选自动附件兜底

开启 `auto_preview=true`，让插件在检测到 publish 后自动尝试发图。

优点：

- 用户体验最容易立刻变好。

缺点：

- 违背当前产品方向：效果图应该是 Agent 的显式交付和反馈动作，不是底层偷偷补发。
- 之前 `sendSessionAttachment` 也受 OpenClaw 限制，非 bundled 插件未必可靠。
- 容易掩盖 Agent 行为没有学会的问题。

### 方案 E：OpenClaw 层增加通用“artifact delivery contract”

和 OpenClaw 同学讨论是否提供通用能力：

- 工具返回 `{ artifacts: [{ path, mime, purpose: "user-visible-preview" }] }`。
- runtime 可以提示或约束模型：带 `user-visible-preview` 的 artifact 不应静默丢弃。
- 当前会话如果是 Telegram/UI，提供统一发送能力和可发送目录。

优点：

- 不是 WeClawBot 特化解法。
- 适合所有“生成图片/图表/PDF/截图后给用户看”的 Agent 场景。

缺点：

- 需要 OpenClaw runtime 级改造。
- 短期不能立刻解决 100 的实测问题。

### 方案 F：在 hook 层做确定性投递（建议优先）

这个方案不来自"怎么让模型记得发图"，而是先承认一个前提：**图回传是确定性动作，不该是模型的可选第二步。**

#### 先破一个矛盾

文档前面把两件事绑在了同一个动作上：

1. 可靠：上屏必须把效果图发回会话。
2. 显式：效果图必须是 Agent 主动的交付（为了反馈/学习），所以反对方案 D 那种"底层偷偷补发"。

0.1.16 / 0.1.17 / 0.1.18 三次失败的本质是同一个：用"让模型可靠地记得多做一步发图"去解决可靠性，而这恰恰是 LLM 最不可靠的地方。只要"发图"是模型可选的第二步，就一定有相当比例漏发。

破法是把两个诉求拆开：

- **像素回传 = publish 的契约副作用。** 用户既然定义了"上屏 = 物理屏 + 会话效果图"（见本文档"关键原则"和"期望行为"），那发图就不是 Agent 的自由发挥，而是 publish 这个动作的组成部分。让它确定性发生，不叫"偷偷补发"，叫"履行 publish 的定义"。这也消解了方案 D 的原则性反对：D 真正该反对的是"用补发掩盖模型没学会版式生成"，但像素回传本来就不需要模型学会。
- **反馈/学习 = 才交给 prompt。** 模型该做的是发完图后用文字邀请纠偏、沉淀偏好。漏发的图是确定性问题，该归 hook；不该漏的反馈对话才是语义问题，归 prompt。

#### 杠杆：conversation hooks（当前已 enabled，但候选方案没用上）

本文档"最小复现线索"里写了"conversation hooks enabled"，这是最直接的确定性投递点：

- hook 监听到一次成功的 screen publish（无论 Agent 走标准 tool 还是自写脚本上屏）；
- 取该次 publish 对应的 preview；
- 直接 attach 到当前会话。

相比方案 D 的 `auto_preview=true`（藏在 publish 内部、且受 bundled 插件限制），hook 在 runtime 层、对 Telegram 和管理 UI 是同一条路径——正好覆盖本文档"两条入口都缺图"的现象。它也不依赖模型读取 `SKILL.md`，整体绕过了 lazy loading 难题。

#### 这把方案 E 的诉求缩小到一个很小的请求

真正要去 OpenClaw 同学那里要的，不是 E 那套宏大的通用 artifact contract，而是一个小东西：**一个稳定的、可作为媒体发送的目录，且 hook 能往里写。** 拿到它，方案 A / B / C 的大半难度都消失（也正是待讨论问题 2、4 的答案：应该暴露，且 `--preview-output-dir` 应默认指向它）。

优点：

- 不依赖模型决策，漏发概率从"经常"降到"只剩通道真故障"。
- 同一条路径同时覆盖 TG 和 UI。
- 短期就能在 100 上验证。

边界：

- 需要 hook 能拿到可发送目录（同 E 的前置，但请求面小得多）。
- 对没有 hook 机制的 Agent（Codex / Claude Code / Gemini CLI）不适用，那些平台才退回到 C/E 的 tool 返回协议 + 文案。承认这个分层：**有 hook 的平台靠确定性，没 hook 的才退回 prompt。**

## 倾向的组合方案

核心结论：**不要再用 prompt 修可靠性。** 把"图回传"做成 publish 的 hook 级确定性副作用（方案 F），把 prompt 省下来只管反馈对话。

短期（今天就能让 100 变好）：

1. 上方案 F 的 hook 投递：publish 成功 → 自动 attach preview，先用一个已知可发送目录把通路验证通。
2. 删掉 `TOOLS.md` 里那条旧示例（"Use ... weclawbot-status-screen as a known-good example for Python/PIL ..."）。它在主动鼓励"自写 PIL 脚本 + CLI + 文字汇报"的坏路径。**先停止教坏，再谈教好**——这比新增规则更重要。
3. 方案 A 作为辅助：补一条"上屏=双交付"的完成定义。但它只是 hook 的兜底语义，不是主修复手段。

中期：

1. 方案 C / `--preview-output-dir`：让 `weclawbotctl screen` 默认把预览输出到可发送目录，使自写脚本路径也天然产出可发送的图，而不是落到 `/tmp` 或 `~/.cache`。
2. 方案 B：安装器写 managed block，范围收到最小——只声明"上屏=双交付"完成定义 + 指向标准 tool，用带版本标记的 `<!-- weclawbot:managed -->` 包裹，可更新可卸载，绝不覆盖用户沉淀。

长期：

1. 与 OpenClaw 讨论方案 E，把"用户可见 artifact 交付"做成通用 Agent 能力——主要是为没有 hook 机制的 Agent（Codex / Claude Code / Gemini CLI）兜底。它对生态正确，但短期解决不了 100，不该排在前面。

## 待讨论问题

> 下面在每个问题后补了一条基于方案 F 的倾向性回答，供讨论时对齐，不是定论。

1. “上屏成功但效果图失败”是否应该算任务失败，还是算部分成功？
   - 倾向：算**失败**并明确报因，不静默降级成"已发到屏上"。做了 hook 投递后，这种情况几乎只剩通道真故障一种，报因成本很低。
2. OpenClaw 推荐的可发送媒体目录是什么？是否应暴露给插件/Agent？该目录或 artifact 机制是否同时覆盖 Telegram 和管理 UI？
   - 倾向：**应该暴露**，且 hook 必须能往里写。这是方案 F 唯一的真正前置，也是把方案 E 缩小成"一个可发送目录"这个小请求的关键。最好同一目录同时覆盖 TG 和 UI。
3. Agent 自写脚本是否应该被允许直接调用 `weclawbotctl screen`，还是应强制走插件 tool？
   - 倾向：**允许，但堵出口不堵入口。** 只要 hook 在 publish 层确定性投递、`--preview-output-dir` 保证产物可发送，就不必强禁自写脚本（也禁不住）。
4. `weclawbotctl screen` 是否应支持一个 `--preview-output-dir` 默认指向 OpenClaw workspace/artifacts？
   - 倾向：**应该**，默认指向问题 2 的可发送目录，让自写脚本路径也天然产出可发送的图。
5. 安装器修改 workspace `AGENTS.md` 是否可接受？如果可接受，managed block 格式如何设计？
6. 是否需要把 `weclawbot-curator` skill 的 description 改得更触发模型主动 read？
7. 对非 OpenClaw Agent，例如 Codex、Claude Code、Gemini CLI、Hermes，应该如何表达同一个效果图交付契约？
   - 倾向：分层。**有 hook 机制的平台靠确定性投递；没有 hook 的才退回到 tool 返回协议 + prompt 文案（方案 C/E）。** 不要指望在每个 runtime 的 prompt 里都把规则讲到位。
8. 用户的版式偏好应该写入哪里：Agent 长期记忆、WeClawBot 本地偏好文件，还是仅通过自然历史沉淀？

## 给讨论同学的最小复现线索

环境：

- 服务器：100
- OpenClaw：2026.6.10
- WeClawBot npm 包：`@openbrt/weclawbotctl@0.1.19`
- 插件 doctor：通过，conversation hooks enabled

复现：

1. Telegram 对 OpenClaw bot 说：
   - “把今天的世界杯赛程做个表格放我屏上”
2. 观察：
   - 真机屏幕更新。
   - Telegram 只收到文字“已发到屏上 ✅ ...”。
   - 没有效果图。
3. 服务器检查：
   - `/tmp/weclawbot-preview-*/*.png` 存在。
   - `~/.cache/weclawbot/*preview.png` 可能存在。
   - session trajectory 中 `didSendViaMessagingTool=false`。
   - `messagingToolSentMediaUrls=[]`。

## 当前不建议的做法

- 不建议把字体、换行、分页策略下沉到固件。
- 不建议把“清屏”继续用黑图/白图/空白图模拟。
- 不建议把 Telegram 发图能力写成 WeClawBot 私有逻辑，除非只是临时诊断。注意：方案 F 的 hook 投递**不属于**这一条——它是经 OpenClaw runtime 的可发送目录/媒体能力投递，履行的是 publish 的契约定义，而不是绕过 runtime 自建一个私有 TG 发送器。
- 不建议只继续改 npm 包内 `SKILL.md` 文案，因为 OpenClaw lazy skill 可能不会被模型读取。
- 不建议继续用 prompt / 强注入去修"可靠性"。prompt 只该负责反馈对话；图回传应当是确定性副作用（方案 F）。
