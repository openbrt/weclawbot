# 缺口事件:自我进化的最小落地方案

本文是 [`self-evolving-architecture-discussion.md`](./self-evolving-architecture-discussion.md) 的具体化。讨论稿描述了四层目标架构(设备 / agent 运行时 / 进化控制面 / skill registry),本文只回答一个问题:**在现有代码上,用最小改动,先把"进化飞轮"的入口建起来。**

结论先行:不要先建 ledger / registry / 灰度发布那一整套。先建一个原语——**缺口事件 (gap event)**,它是整条弧线 `多用户长尾 → skill 沉淀` 的唯一入口。其余三层继续当北极星挂着。

## 为什么是这一个原语

整条商业弧线是 `开源 → 多用户长尾 → skill 沉淀 → 确定性产品 → 商业化`。其中:

- LLM-in-loop 不是产品,是**发现机**——存在意义是暴露"哪些场景还没被确定性规则覆盖";
- 一旦某场景沉淀成 skill,这条流量就该从模型退回便宜的确定性路径(这就是商业化的毛利故事);
- 所以真正稀缺、且越早建越值钱的,是**"看见缺口"的能力**,不是发布机器。

而当前系统恰恰看不见缺口。`server/curator-proxy/server.mjs` 写的 `logEvent("decision", …)` 是运维 trace:它记录了"做了什么决策",但不记录"这个决策对不对、用户事后怎么反应"。最危险的是 **false-negative 不可见**——被 `ignore` 掉但用户其实想要的消息,不产生任何事件,就这么消失了。一个只从"用户主动纠正"学习的系统,会沿着"越来越保守 → 越来越没用"的梯度滑下去。

## 信号分类法:捕获和打标必须分开

用户的事后动作是模糊的,设备/代理**无法**在捕获时就判定语义。所以原则是:**捕获时只记录原始动作 + 时序上下文,语义标签留到人工评审时再打。**

| 标签 | 含义 | 该进哪种 eval |
| --- | --- | --- |
| `false_negative` | 被 ignore/clarify/service_required,但用户想要上屏 | routing/分类样例 |
| `content` | 上屏了但内容/判断错 (`修改为…`) | routing + prompt 样例 |
| `render` | 内容对,但标题/格式/优先级错 | renderer 样例 |
| `false_positive` | 上屏了但根本不该上 (秒清) | routing 负样例,**低信任** |
| `expired` | 清屏只是因为过期,**不是错误** | 不进 eval |
| `not_a_bug` | 评审后认为决策正确 | 丢弃,但保留为回归正样例 |

注意 `false_positive` 和 `expired` 在捕获层长得一模一样(都是"清屏"),**只能靠人区分**。这就是为什么打标不能自动化。

### false-negative 的服务端代理信号

false-negative 平时不可见,但有一个 **代理服务端就能观测、零固件改动** 的启发式:

> **同一 sender 在一次 `ignore` 之后短时间内重发了高度相似的文本** = 强 false-negative 候选。

用户期望某事发生、屏幕没反应、于是又试一次——这个"重发"模式正好暴露了被吞掉的缺口。这是现在就能抓的最有价值的一类信号。

## 三个改动点

### 改动 1 — 代理层:缺口账本 (server only,无固件依赖)

文件:`server/curator-proxy/server.mjs`

`server.mjs` 是唯一看得见"每个 event → 每个 decision"的咽喉点,在这里加捕获,不动设备、不动 SCF。

1. 维护一个**按 sender_ref 的近期决策环**(内存 + 落一个小文件以扛重启),每条存:`{event_id, action, note?, skill, confidence, text_hash, text, ts}`。复用现有 `logEvent("decision", …)` 已经算出来的字段即可。
2. 每个进来的 event,先判它是不是**反馈型**:
   - 命中 `修改为/改为/改成` → `trigger.type = "correction"`;
   - 是 `/clear`(若由设备转发到云端的话)→ `trigger.type = "rapid_clear"`;
   - 文本与该 sender 最近一条 `ignore` 决策的 `text_hash` 高度相似且在窗口 W 内 → `trigger.type = "recapture"`(即上面的 false-negative 代理信号)。
3. 命中则向**独立**的 `gap-ledger.jsonl` 追加一条缺口事件(schema 见下),`review_state = "pending"`、`label = null`。
4. **缺口账本和运维 log 物理分开**:运维 log 继续 `LOG_PATH`;缺口账本走 `GAP_LEDGER_PATH`,权限更严、保留策略独立——因为它含原始微信文本。

窗口 W、相似度阈值用环境变量,默认保守(比如 W=10min,相似度=归一化后前缀/编辑距离)。

### 改动 2 — 固件:精确反馈 + 回捞命令 (小改动,Phase B)

改动 1 的关联是**启发式**(靠 sender + 时序猜)。要做到精确,设备在发反馈时带上溯源:

- 在反馈型上行里加 `in_reply_to: <上一条 event_id>` 和 `feedback_type`,代理就不用猜了。
- 新增一个**显式回捞命令**喂 false-negative:用户在微信里对刚被 ignore 的消息回 `+` / `贴上`,设备据此发一条 `feedback_type = "recapture"` 的事件。这是把"被吞掉的缺口"变成主动信号的唯一干净办法。

这一步可以晚做;改动 1 的启发式先把飞轮转起来。

### 改动 3 — 运行时:评审 CLI + 强制门禁

文件:新增 `runtime/src/cli/review.mjs`;改 `runtime/package.json`

1. `npm run review` 读 `gap-ledger.jsonl` 里 `review_state = "pending"` 的条目,逐条展示"原始文本 / 当时决策 / 触发动作",让人:
   - 打标签(上面六类之一);
   - 对要沉淀的,写出**最小可复现文本 + 期望 `expect` 块**;
   - 追加到 `runtime/eval/sticky-core.jsonl`(格式已存在,无需改 `eval.mjs`);
   - 把该缺口标 `review_state = "promoted" | "rejected"`。
2. **隐私边界就落在这一步**:原始文本只在私有缺口账本里;流出去的(进 eval、将来开源)是**人工脱敏后的样例**。eval case = 讨论稿里说的"贡献而非挖矿"的贡献单元。
3. 门禁:在 `scripts/package-scf.sh`(打 SCF 包前)强制跑 `npm run eval`,不过不让打包。`npm run eval` 现成,只是现在没人强制。

## 缺口事件 schema

私有、本地、含原始文本:

```jsonc
{
  "gap_id": "gap_3f2a…",
  "ts": "2026-06-08T12:00:00Z",
  "sender_ref": "<hash>",          // 不存微信身份明文
  "event_id": "local_ab12cd34",    // 关联的那次决策
  "observed": {                    // 当时 curator 做了什么(复用 decision 日志字段)
    "action": "ignore",
    "skill_id": "sticky-core",
    "skill_version": "0.1.3",
    "confidence": 0.96,
    "note": null
  },
  "trigger": {                     // 触发缺口的用户动作
    "type": "recapture",           // correction | rapid_clear | recapture | manual
    "text": "+",                   // 原始,私有
    "delta_ms": 8000               // 距上次决策的时间
  },
  "original_text": "…",            // 原始 event 文本,私有
  "privacy_state": "raw",          // raw → (评审脱敏) → redacted
  "review_state": "pending",       // pending | promoted | rejected
  "label": null                    // 评审时打:false_negative | content | render | false_positive | expired | not_a_bug
}
```

promote 后写入的 eval case(沿用现有格式,可公开):

```json
{"name": "recapture: 取件码被误 ignore", "text": "丰巢 8-2-3306", "expect": {"action": "create_note", "title": "取件"}}
```

## 分期

- **第一刀(纯 server,最高杠杆):** 改动 1 + 改动 3。当天就能开始攒缺口、当周就能 review 出第一批 eval、当场就能把 `npm run eval` 设成发布门禁。数据量小也值得马上建,因为它越早建越值钱。
- **第二刀(小固件):** 改动 2,把启发式关联升级成精确溯源 + 显式回捞。
- **明确不做(现在):** 中心化交互账本全量、skill registry / 签名 / marketplace、`stable/beta` channel、灰度发布、`update_note`/`merge_note` 之外的 action 扩展。单设备/早期阶段这些是过度设计,等缺口流稳定、确定性覆盖率有数据后再说。

## 当前落地状态

已完成:

- `server/curator-proxy/server.mjs` 已加入代理层 gap capture。
- proxy 会维护按 `sender_ref` hash 分组的近期决策缓存:
  - 默认路径: `/var/log/weclawbot/curator-recent-decisions.json`
  - 环境变量: `RECENT_DECISIONS_PATH`
- proxy 会把行为信号写入私有 gap ledger:
  - 默认路径: `/var/log/weclawbot/precipitation/gap-events.jsonl`
  - 环境变量: `GAP_LEDGER_PATH`
- 已支持的捕获信号:
  - `recapture`: 同一 sender 在一次 `ignore` 后 10 分钟内重发相似文本,写 `type=false_negative`;
  - `correction`: `修改为/改为/改成...`,写 `type=content`,并带用户修正后的 `truth`;
  - `rapid_clear`: `/clear`,写 `type=false_positive`。
- 固件已支持 `kind=wechat_feedback` 上行:
  - `feedback_type=correction`: 本地 `修改为...` 即时替换当前微笺,同时送入 gap ledger;
  - `feedback_type=rapid_clear`: 本地 `/clear` / `/clear all` 即时清屏,同时记录 false-positive 候选;
  - `feedback_type=recapture`: 用户发送 `+` / `贴上` 时,代理从最近一次 `ignore` 回捞原文并贴屏,同时记录 false-negative 候选。
- 低价值重复消息会被过滤,如 `你好`、`测试`、`收到`、`谢谢`,避免污染 false-negative 候选。
- 运维日志只记录 `gap_event` 的类型、原 event、相似度和延迟,不记录原文。
- 原文只进入私有 gap ledger。
- `runtime/scripts/package-scf.sh` 已在打包前强制 `npm run --silent eval`。
- SCF zip 已包含 `runtime/skills/`,因此空的 `Skill Library` 目录和后续沉淀样例会进入云函数包。
- 更新后的 proxy 已部署到 `weclawbot.link`。
- 更新后的 SCF runtime 已部署到 `ap-guangzhou/default/weclawbot-curator`。

已验证:

- 本地 mock 上游下,重复发送 `丰巢 8-2-3306` 会生成 `false_negative` gap event。
- 本地 mock 上游下,`修改为 丰巢 8-2-3306 今天 18 点前取` 会生成 `content` gap event。
- 本地 mock 上游下,`kind=wechat_feedback` 三类显式反馈均通过:
  - `+` / `贴上` 生成 `false_negative:recapture` 并直接回捞成 `create_note`;
  - `修改为...` 生成 `content:correction`;
  - `/clear` 生成 `false_positive:rapid_clear`。
- 线上重复 `你好` 不会生成 gap event。
- `npm run check`、`npm run eval`、`npm run package:scf` 均通过。

仍未完成:

- 新增的 `kind=wechat_feedback` 还需要线上部署和真机验证。
- `recordGapEvent()` 和服务器 gap ledger 已对齐 schema,但还没有部署一个定时任务把服务器上的 gap ledger 拉进 `runtime` 并执行 `npm run precipitate`。
- SCF 适配器已能从环境变量组装 `rules -> ModelScope student -> DeepSeek teacher` 级联。
- 线上 SCF 已打开 student/teacher 环境变量:
  - student: ModelScope `Qwen/Qwen3.5-27B`;
  - teacher: DeepSeek `deepseek-v4-flash`;
  - SCF timeout: 30s;
  - proxy upstream timeout: 35s。
- 代理日志已经记录 `trace`,可看到规则、student、teacher 的命中路径。
- 微信端反馈策略已改为:用户发来的消息如果没有上屏,也必须给微信回复原因或改写建议。`ignore` 不再默认静默;proxy 会为没有 `user_reply` 的 `ignore` 补一条"这条没有贴到屏幕..."的产品说明。
- 显式回捞命令 `+` / `贴上` 已在固件与代理层实现,待线上部署和真机验证。

### Zero Skill Library

当前约定是 **从零 Skill Library 开始**:

- `runtime/skills/library/sticky-core.jsonl` 初始为空;
- prompt 不再内置取件码或问候语 few-shot;
- 模型只能依赖体裁 policy、schema、以及后续由行为确认沉淀出的样例;
- `sticky-core` 规则模块仍保留,它不是已沉淀的 library,而是安全底座和离线可用路径。

这样避免一开始就把人工写死的样例伪装成"自我进化"成果。

### ModelScope Student

魔搭探索线程确认可用的免费模型可以作为徒弟推理层。当前默认配置:

- `STUDENT_ENABLED=true` 时启用徒弟层;
- `MODELSCOPE_API_KEY` 或 `STUDENT_API_KEY` 提供访问令牌;
- `MODELSCOPE_BASE_URL` 默认为 `https://api-inference.modelscope.cn/v1`;
- `MODELSCOPE_MODEL` 默认为 `Qwen/Qwen3.5-27B`;
- `STUDENT_MIN_CONFIDENCE=0.82` 控制规则低于多少置信度才进徒弟;
- `TEACHER_ENABLED=true` 后,徒弟低于 `TEACHER_MIN_CONFIDENCE` 才进 DeepSeek 老师。
- 非流式返回空内容时会自动走一次流式重试,适配魔搭免费模型的偶发空 `choices`。
- student 不允许把规则层已经判定的 `create_note` 直接降级成 `ignore`/`clarify`/`service_required`;这种分歧会被标记为 `tier_rejected:student:note_downgrade`,并保留规则贴或继续交给 teacher。
- student 也不允许把规则层的 `clarify` 降级成静默 `ignore`;这种分歧会被标记为 `tier_rejected:student:clarify_to_ignore`,保留追问回复。

模型层不直接写入 Skill Library。只有 teacher verdict 被后续行为确认,或用户显式修正形成 gap event 后,才允许沉淀成共享样例。

## 一句话

讨论稿把"可进化"理解成"基础设施齐全";真正的瓶颈在"信号质量"和"看见 false-negative"。缺口事件就是先把这个入口建起来的最小原语——它同时是 eval 的原矿、是开源贡献的单元、是确定性覆盖率的度量基准。
