# 项目发布检查项

发布 WeClawBot 项目时必须执行本清单。不要把“代码已推上去”当成发布完成；
发布完成的定义是：公开仓库、GitHub Release、GitHub Pages 安装/配置页、固件
manifest、固件二进制和项目文案全部一致，并且没有私有内容泄漏。

适用范围：

- `openbrt/weclawbot` 的 `main` 更新；
- 固件 tag / GitHub Release；
- GitHub Pages 上的安装/配置页；
- npm Agent 插件相关公开文档；
- 项目定位文案更新。

## 0. 发布前提

- [ ] 已阅读 [public-release-boundary.md](public-release-boundary.md)。
- [ ] 明确本次发布类型：
  - [ ] 仅文档/文案；
  - [ ] 安装/配置页；
  - [ ] 固件 release；
  - [ ] Agent 插件/npm；
  - [ ] 官网/仿真机 contract；
  - [ ] 多项同时发布。
- [ ] 本地分支基于远端公开分支，不覆盖远端历史：

  ```sh
  git fetch origin main
  git status --short
  git log --oneline --decorate -5
  ```

## 1. 公开边界检查

- [ ] 暂存区只包含公开边界内的文件：

  ```sh
  git diff --cached --name-only
  ```

- [ ] 没有暂存私有目录、生成目录或本机状态：

  ```sh
  git diff --cached --name-only | rg '(^server/|^site-experience/|^history/|^build/|node_modules|__pycache__|\.env$|\.log$|runtime/dist/)'
  ```

  期望：无输出。

- [ ] 没有暂存密钥/token/password/private key 形态：

  ```sh
  git grep --cached -n -I -E '(npm_[A-Za-z0-9]|BEGIN [A-Z ]*PRIVATE KEY|PRIVATE KEY|AKIA[0-9A-Z]{16}|api[_-]?key[[:space:]]*[=:][[:space:]]*['"'"'"][A-Za-z0-9_\-]{16,}|secret[[:space:]]*[=:][[:space:]]*['"'"'"][A-Za-z0-9_\-]{16,}|token[[:space:]]*[=:][[:space:]]*['"'"'"][A-Za-z0-9_\-]{16,}|password[[:space:]]*[=:][[:space:]]*['"'"'"][^*][^'"'"'"]{6,})'
    -- ':!docs/project-release-checklist.md'
  ```

  期望：无输出。

- [ ] whitespace 检查通过：

  ```sh
  git diff --cached --check
  ```

## 2. 项目定位文案检查

本项目最新定位是：

> WeClawBot 是微信官方智能体和用户自带 Agent 都能接管的开源桌面屏。

发布前检查：

- [ ] GitHub About 描述和 homepage 是最新定位：

  ```sh
  gh repo view openbrt/weclawbot --json description,homepageUrl,repositoryTopics,url
  ```

  期望：
  - `description` 提到微信官方智能体和自定义 Agent；
  - `homepageUrl` 是 `https://weclawbot.link/`；
  - topics 包含 `esp32`、`mqtt`、`agent` 等公开检索词。

- [ ] 明确区分官网和安装页：
  - `https://weclawbot.link/` 是项目官网和仿真机交互体验网站；
  - `https://openbrt.github.io/weclawbot/web/` 是固件安装与设备配置页面；
  - GitHub About homepage 指向官网，不指向安装页；
  - README、GitHub Pages 项目首页和官网中都应有安装/配置入口。

- [ ] `README.md` 第一屏讲清楚 Official 和 BYOA 两条路径。
- [ ] `README.zh-CN.md` 第一屏讲清楚“固件安装与设备配置”和两种接管方式。
- [ ] `README.md` 和 `README.zh-CN.md` 标题下方有安装/配置直达链接。
- [ ] `index.html` 顶部导航和第一屏都有明确的 `固件安装 / 设备配置` 入口。
- [ ] `web/index.html` 标题明确是固件安装与设备配置，不是只像文档页。
- [ ] 没有过期定位词或旧版本号混进用户入口：

  ```sh
  rg -n '官方云端|本地控制台|本地产物|0\.1\.46|0\.1\.47' README.md README.zh-CN.md index.html web/index.html web/app.js
  ```

  允许项必须逐条解释；否则应改掉。

## 3. Web 安装/配置页检查

- [ ] 本地语法检查：

  ```sh
  node --check web/app.js
  ```

- [ ] 线上 Pages 首页能看到安装/配置入口：

  ```sh
  curl -fsSL -H 'Cache-Control: no-cache' 'https://openbrt.github.io/weclawbot/?ts=check' \
    | rg -n '固件安装|设备配置|web/'
  ```

- [ ] 线上安装/配置页能看到刷写按钮、配置标题和两种接管方式：

  ```sh
  curl -fsSL -H 'Cache-Control: no-cache' 'https://openbrt.github.io/weclawbot/web/?ts=check' \
    | rg -n '固件安装|设备配置|安装 / 更新固件|WeClawBot 官方|自定义智能体'
  ```

- [ ] 浏览器人工检查一次：
  - [ ] 桌面 Chrome/Edge 可打开 `https://openbrt.github.io/weclawbot/web/`；
  - [ ] 页面左侧是固件安装；
  - [ ] 页面右侧是设备配置；
  - [ ] 串口连接按钮可见；
  - [ ] 接管方式是 `WeClawBot 官方` / `自定义智能体`；
  - [ ] 危险操作折叠在高级区域。

## 4. 固件版本与 release 检查

如果本次发布包含固件，设定：

```sh
VERSION=0.1.77
```

发布前：

- [ ] `main/app_config.h` 版本与 `VERSION` 一致。
- [ ] `web/firmware-contract.json` 的 `firmware.version` 与 `VERSION` 一致。
- [ ] 公开版构建关闭 `CONFIG_WEC_DEVELOPMENT_BUILD`：

  ```sh
  rg -n 'CONFIG_WEC_DEVELOPMENT_BUILD' sdkconfig.defaults sdkconfig.release.defaults main/Kconfig.projbuild
  ```

- [ ] release workflow 使用公开 defaults：

  ```sh
  rg -n 'sdkconfig.defaults;sdkconfig.release.defaults|Publish installer firmware to Pages source' .github/workflows/release.yml
  ```

触发 release：

```sh
gh workflow run "Release Firmware" --repo openbrt/weclawbot --ref main -f version="$VERSION"
gh run watch --repo openbrt/weclawbot --exit-status
```

发布后必须检查：

- [ ] GitHub Release 存在且资产完整：

  ```sh
  gh release view "v$VERSION" --repo openbrt/weclawbot \
    --json tagName,name,publishedAt,url,assets \
    --jq '{tagName,name,publishedAt,url,assets:[.assets[].name]}'
  ```

  必须包含：
  - [ ] `manifest.json`
  - [ ] `weclawbot-$VERSION.bin`
  - [ ] `weclawbot-$VERSION-bootloader.bin`
  - [ ] `weclawbot-$VERSION-partition-table.bin`
  - [ ] `weclawbot-$VERSION-ota-data-initial.bin`
  - [ ] `weclawbot-$VERSION-firmware-contract.json`
  - [ ] `weclawbot-$VERSION-web-installer.zip`
  - [ ] `release-notes.md`

- [ ] `main` 被 workflow 回写到安装页固件提交：

  ```sh
  gh api repos/openbrt/weclawbot/commits/main \
    --jq '{sha: .sha, message: .commit.message, date: .commit.author.date}'
  ```

  期望 commit message 类似 `Publish web installer firmware v$VERSION`。

- [ ] 线上 manifest 是新版本：

  ```sh
  curl -fsSL -H 'Cache-Control: no-cache' "https://openbrt.github.io/weclawbot/web/manifest.json?ts=$VERSION"
  ```

  期望 JSON 中 `"version": "$VERSION"`。

- [ ] 线上 contract 是新版本、公开 channel、非开发版：

  ```sh
  curl -fsSL "https://raw.githubusercontent.com/openbrt/weclawbot/main/web/firmware-contract.json" \
    | rg -n "\"version\": \"$VERSION\"|\"channel\": \"public\"|\"development_build\": false"
  ```

- [ ] 线上固件二进制包含新版本字符串，不包含旧版本字符串：

  ```sh
  curl -fsSL https://raw.githubusercontent.com/openbrt/weclawbot/main/web/firmware/weclawbot.bin \
    -o "/tmp/weclawbot-$VERSION.bin"
  shasum -a 256 "/tmp/weclawbot-$VERSION.bin"
  strings "/tmp/weclawbot-$VERSION.bin" | rg "$VERSION"
  strings "/tmp/weclawbot-$VERSION.bin" | rg '0\.1\.46|0\.1\.47' && false || true
  ```

- [ ] Pages build 成功：

  ```sh
  gh run list --repo openbrt/weclawbot --workflow pages-build-deployment --limit 3
  ```

## 5. 真机烟测

能接触真机时，固件 release 后至少跑一遍：

- [ ] 用公开安装页刷机，不擦 NVS。
- [ ] 配置页连接设备后版本显示为本次 `VERSION`。
- [ ] 已保存 Wi-Fi 不回显明文密码，只显示“已保存，留空不修改”。
- [ ] `WeClawBot 官方` 保存并重启后进入微信二维码/官方入口。
- [ ] `自定义智能体` 保存并重启后显示六位配对码，不出现“扫码”类文案。
- [ ] 配置页 **重置配置 -> 重置智能体配对** 后，设备能重新显示 BYOA 配对码。
- [ ] BYOA 从一个 Agent 重新配对到另一个 Agent 后，新 Agent 能上屏，旧 Agent
  再发上屏被云端明确拒绝，且真机屏幕不被旧 Agent 改动。
- [ ] 旧 OpenClaw/Hermes Agent 的本地凭据失效时，直接上屏请求在生成脚本、
  渲染图片或拉取数据前快速失败，并提示重新配对。
- [ ] 左右键短按能翻日历/照片/留言页。
- [ ] 清文字不把照片清掉；全清才清照片、微信和 Agent 状态。
- [ ] BYOA 下微信输入被忽略。
- [ ] BYOA Agent 发布 `thinking` 后能回落 idle 或被新 screen document 覆盖。
- [ ] BYOA `screen_clear` 是清屏动作，不允许用黑图/白图模拟清屏。

## 6. Agent/npm 检查

如果发布涉及 `@openbrt/weclawbotctl`：

- [ ] package version 更新。
- [ ] npm README 第一屏说明“一句话让 Agent 安装并配对”。
- [ ] OpenClaw install/doctor 文档与实际命令一致。
- [ ] README、OpenClaw skill/workspace 明确 direct screen request 先跑
  `weclawbotctl doctor --online --timeout 8` 或 `weclawbot_status online:true`；
  `credential_revoked_or_not_current_owner` 必须停止并提示重新配对。
- [ ] README、Hermes skill、OpenClaw skill 都明确 `mono1` 位语义：
  `1=黑墨`、`0=白纸`，Pillow mode `1` 需要只把黑像素打包成 `1`。
- [ ] skill 文档明确普通首屏默认白底黑字、低墨量，不默认黑底白字。
- [ ] 测试通过：

  ```sh
  npm run check --prefix integrations/openclaw/weclawbot-openclaw-plugin
  ```

- [ ] npm 发布后确认：

  ```sh
  npm view @openbrt/weclawbotctl version
  npm view @openbrt/weclawbotctl dist.tarball
  ```

- [ ] 在至少一个真实 Agent 环境验证：
  - [ ] `weclawbotctl bind <code>`
  - [ ] `weclawbotctl doctor --online --timeout 8`
  - [ ] `weclawbotctl thinking`
  - [ ] `weclawbotctl screen`
  - [ ] 用户会话能收到效果预览图，不能只收到“已发送”。
  - [ ] 首次诗词/卡片上屏不是反相黑底白字；如用户未要求反白，预览不出现
    大面积黑底。

## 7. 文档和讨论记录

- [ ] README、中文 README、Pages 首页、安装/配置页文案同步。
- [ ] `docs/current-progress.md` 记录本次发布日期、版本、结果和剩余风险。
- [ ] 如果 release 行为改变了官网仿真机、按键、QR、配对码、页面数量或 Agent
  能力，更新 `web/firmware-contract.json`。
- [ ] 如果新增失败模式或可靠性结论，更新对应讨论文档，而不是只留在聊天记录。

## 8. 完成定义

只有全部适用项通过，才能说“发布完成”：

- [ ] 公开仓库 `main` 已推送。
- [ ] GitHub Release 已发布。
- [ ] Pages 安装/配置页可访问。
- [ ] Pages manifest 指向本次版本。
- [ ] raw 固件二进制确认是本次版本。
- [ ] 项目文案无过期定位。
- [ ] 没有私有内容进入公开仓库。
- [ ] 已记录未完成项和风险。

如果任何一项失败，发布状态是“未完成”，需要继续修，不能交给用户去发现。
