# Hermes 首次上屏流程复盘

日期：2026-07-01

## 背景

用户在本机 Hermes 中完成 `@openbrt/weclawbotctl` 安装和六位配对后，第一次
要求“发一首李白的诗上屏，用书法体显示”。最终真机显示为黑底白字、字形发虚，
处理时间接近一小时，初次体验很差。

## 过程观察

- 安装、配对、`status` 和在线诊断均成功，MQTT/WSS 不是本次失败点。
- Hermes 多次进入需要用户确认的 shell / code 工具调用；用户没有及时确认时，
  工具超时，Agent 又继续绕路重试，导致流程被拉得很长。
- 后续 Hermes 自动生成了本地 `weclawbot-screen` skill 和一个诗词渲染模板，
  说明它试图把经验沉淀下来，但沉淀内容里带入了协议错误。
- 首次诗词模板使用白底黑字的 PIL 预览图，但打包到
  `weclawbot.screen_document.v1` 时把白像素写成 `1`，导致真机显示反相。

## 根因

WeClawBot 固件显示路径把 packed mono1 的 bit `1` 解释为黑墨，bit `0` 解释为
白纸：

```cpp
const bool black = (row[x >> 3] & (0x80 >> (x & 7))) != 0;
```

而本地 Hermes skill 模板把 Pillow mode `1` 的白像素打包成 `1`。Pillow mode
`1` 的源像素是 `0=black`、`1=white`，所以正确打包应当只在源像素为黑色时设置
输出 bit。

这不是固件审美问题，也不应该通过固件写死版式解决。固件边界是“接收 bounded
pixels 并显示”；审美、分页、预览、用户偏好学习应留在 Agent skill / tool 层。

## 已修正

- 修正本地 Hermes `weclawbot-screen` skill 的 mono1 位语义说明。
- 修正本地诗词模板：`0=black` 的 Pillow 像素才打包为 bit `1`。
- 给本地模板增加高墨量保护，普通屏幕超过约 45% 黑像素时拒绝发布，避免首屏
  再次变成大面积反白。
- 在公开 OpenClaw/Hermes 插件文档中加入默认首屏视觉基线：
  白纸背景、黑墨、可读边距、低墨量；除非用户明确要求反白/夜间/黑底海报，
  不要默认黑底白字。
- 在 `weclawbot_validate_screen_document` 中加入高墨量 warning，不阻止用户
  明确要求的风格，但提醒 Agent 先预览和自检。

## 后续建议

- 真实 Agent 首次安装后的 smoke test 必须包含“诗词/卡片上屏”并核对真机不是
  黑底白字。
- Agent 长流程应优先使用已安装的 `weclawbotctl` 和 skill 模板，减少临时写脚本
  与需要人工确认的工具调用。
- 效果预览图仍是用户和 Agent 共同学习审美偏好的关键反馈面；上屏成功但不给
  用户看预览，仍视为交付不完整。
