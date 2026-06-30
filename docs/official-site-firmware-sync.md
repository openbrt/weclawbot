# 官网仿真机与公开固件同步

## 目标

`weclawbot.link` 的网页仿真机要像公开版固件一样工作，而不是复制一套
容易过期的按键、二维码和屏幕规则。官网部署在 GitHub 私有仓库；公开仓库
只包含可审计的固件和不含用户数据的行为契约。

## 唯一行为来源

公开仓库 `openbrt/weclawbot` 的
`web/firmware-contract.json` 是网页仿真机的兼容性契约。

契约包含：

- 产品名称和公开版/开发版屏幕标题；
- 设备 profile、屏幕尺寸、像素格式和内容区；
- 二维码有效期、轮询周期、自动翻页和屏保时长；
- 页脚与状态图标约定；
- 左、中、右按键的短按/长按命令及阈值。

它不包含 Wi-Fi 凭证、微信 token、用户消息、云端整理器提示词、腾讯云
资源 ID 或部署密钥。

## 发布路径

```text
公开固件 PR
  -> firmware + web/firmware-contract.json 同时修改
  -> GitHub tag / Release
  -> Release 附带 versioned contract
  -> 私有官网 CI 拉取并做 schema 校验
  -> weclawbot.link 部署仿真机
```

运行中的官网服务默认读取：

```text
https://raw.githubusercontent.com/openbrt/weclawbot/main/web/firmware-contract.json
```

再通过同源的 `/api/firmware-contract` 交给浏览器。这样浏览器不直接访问
GitHub，也不需要跨域权限。官网服务每十分钟刷新一次；拉取失败时保留上一份
已验证的契约，而不是退回到未知行为。

## 私有仓库 CI 要求

部署 workflow 在 build 前应：

1. 下载 `FIRMWARE_CONTRACT_URL`；
2. 检查 `schema` 为已支持的 `firmware-contract.v1`；
3. 验证 `display.width`、`display.height`、按键命令和定时字段存在；
4. 对照仿真机的本地回环测试：二维码、自动刷新、自动翻页、左右键短按、
   左右键长按和中键长按；
5. 只在验证通过后部署。

正式部署仍由私有仓库的 GitHub Actions 使用 `DEPLOY_SSH_KEY`、
`DEPLOY_HOST`、`DEPLOY_USER` 等私有 secret 完成；这些 secret 永远不进入
公开固件仓库。

## 版本策略

- 日常运行：读取 `main`，让官网自动跟上已经合入的公开行为修复。
- 可复现演示：将 `FIRMWARE_CONTRACT_URL` 固定为某个 tag 对应的 release asset。
- 不兼容 schema：私有网站 CI 拒绝发布；运行中的站点继续使用最后一份兼容
  契约并记录告警。
- 私有/本地开发固件可以显示 `微笺（开发版）` 并使用内部递增版本号；官网和
  公开安装页只承诺公开仓库的 contract 与公开 release。不要把私有开发固件号
  当成公开用户可安装版本写进官网文案。
