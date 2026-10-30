# 配置参考：setting.json / identity.json

> 适用版本：v0.7.0-rc6-dev（记忆库 v9 / 记忆 v5 R3）
> 更新日期：2026-10-31
> 关联：[README](../README.md)、[ARCHITECTURE](ARCHITECTURE.md)、[MEMORY_V5_R3_BASELINE](MEMORY_V5_R3_BASELINE.md)

---

## 0. 先读这几条

1. `setting.json` 和 `identity.json` 都是**标准 JSON**，不支持 `//`、`/* */` 注释，也不支持尾随逗号。
2. `setting.json` 由 `ConfigManager::saveSetting()` 按固定字段重写；即使手工加入注释或未知字段，设置界面保存后也会丢失。
3. 大部分配置在**启动时加载**；修改后请重启应用。`identity.json` 支持 `reloadIdentity()`，普通用户仍建议重启。
4. 不要把 `deepseek_api_key`、QQ `access_token`、真实 QQ 号提交到 git。
5. 本文只描述当前代码实际读取的字段；未列出的字段不会生效。

路径：

```text
app_data/config/setting.json
app_data/config/identity.json
```

---

## 1. setting.json

### 1.1 最小结构

```json
{
    "api": {
        "deepseek_api_key": "sk-placeholder",
        "gpt_sovits_url": "http://127.0.0.1:9880"
    },
    "memory": {
        "short_term_length": 15,
        "summary_length": 15,
        "short_context_enabled": true,
        "long_term_memory_enabled": true,
        "debug_full_logs": false,
        "agent_enabled": true,
        "agent_trigger": "memory_question"
    },
    "time": {
        "night_start": "22:00",
        "night_end": "07:00",
        "blush_lasting": 30,
        "emotion_lasting": 15,
        "distance_lasting": 15
    },
    "qq": {
        "enabled": false,
        "ws_url": "ws://127.0.0.1:3001",
        "access_token": "",
        "reply": {
            "private": "master",
            "private_allow": [],
            "group": "none",
            "group_allow": [],
            "master_any_group": false
        }
    },
    "tts": {
        "mode": "api"
    }
}
```

### 1.2 api

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `deepseek_api_key` | `sk-placeholder-key` | DeepSeek API Key；也可以在设置界面填写 |
| `gpt_sovits_url` | `http://127.0.0.1:9880` | GPT-SoVITS HTTP API 地址 |

### 1.3 memory：短期上下文与长期记忆

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `short_term_length` | `15` | 每次对话携带的最近轮数 |
| `summary_length` | `15` | 未摘要对话达到该轮数时触发长期记忆提取；小于 1 时回退 15 |
| `short_context_enabled` | `true` | 是否读写 `chat_history`；关闭后允许回复时走 ephemeral，不落短期上下文 |
| `long_term_memory_enabled` | `true` | 是否写 `raw_events` / interactions / claims，是否触发蒸馏 |
| `debug_full_logs` | `false` | 是否输出完整消息、prompt、回复、提取正文；默认关闭，排查后请改回 |

> 短期上下文与长期记忆是两套独立开关。关闭长期记忆不等于不写 `chat_history`；关闭短期上下文也不等于不能回复。

### 1.4 memory：interaction 分段

| 字段 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `qq_group_idle_minutes` | `15` | 1–1440 | 群聊空闲多久后触发批次处理 |
| `qq_group_batch_size` | `50` | 1–500 | 单批最多处理事件数 |
| `interaction_gap_seconds` | `90` | 10–3600 | 相邻消息并入同一 interaction 的最大间隔 |
| `interaction_max_events` | `20` | 1–100 | 单 interaction 最大事件数 |
| `interaction_max_minutes` | `10` | 1–120 | 单 interaction 最大时间跨度 |

### 1.5 memory：recent_group_context

```json
"recent_group_context": {
    "enabled": true,
    "max_age_minutes": 30,
    "candidate_limit_per_group": 50,
    "max_owner_groups": 2,
    "targeted_item_limit": 10,
    "targeted_char_budget": 1200,
    "overview_item_limit": 50,
    "overview_char_budget": 2200,
    "hot_window_items": 10,
    "warm_line_max_chars": 24
}
```

| 字段 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `enabled` | `true` | bool | 是否启用近期群聊检索 |
| `max_age_minutes` | `30` | 1–1440 | 只检索最近多少分钟的原始消息 |
| `candidate_limit_per_group` | `50` | 1–500 | 每群候选上限 |
| `max_owner_groups` | `2` | 1–20 | Owner 私聊/pet 最多读取几个最近活跃群 |
| `targeted_item_limit` | `10` | 1–100 | 定向检索注入条数 |
| `targeted_char_budget` | `1200` | 100–8000 | 定向检索字符预算 |
| `overview_item_limit` | `50` | 1–500 | 泛问候选条数 |
| `overview_char_budget` | `2200` | 100–12000 | 泛问字符预算 |
| `hot_window_items` | `10` | 1–100 | 泛问热窗口条数 |
| `warm_line_max_chars` | `24` | 8–200 | 温窗口单行截断长度 |

### 1.6 memory：MemoryAgent（PR-4）

| 字段 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `agent_enabled` | `true` | bool | 是否允许 QQ 记忆问句使用只读检索 Agent；pet 永不进入 |
| `agent_plan_v2` | `true` | bool | v2 多 need Planner/Resolver/Executor；false 回退旧 tool_calls 路径 |
| `agent_trigger` | `"memory_question"` | `memory_question` / `always` | 仅记忆问句触发，或所有 QQ 回复都尝试 |
| `agent_timeout_ms` | `6000` | 1000–15000 | 单次 Agent 总超时；超时保留已授权部分结果并回退 top-k |
| `agent_max_rounds` | `2` | 1–4 | planner 最多工具轮数 |
| `agent_max_results` | `8` | 1–20 | 单次 Agent 累计结果上限 |

> Agent 只读 `memory_search` / `memory_read` / `memory_related`；工具参数不含 viewer/owner/location/allowed_groups。审计只记录工具名、参数 hash、结果数、耗时和状态，不记录 query/记忆原文。
>
> v2 Planner 只输出 `time.expression` 和 `target`（message_time/event_time/fact_validity），不输出 ISO。`MemoryTimeResolver` 独立解析每个表达，半开区间，最大 31 天；无法解析返回 `unresolved`。`event_time` 无独立索引时按 message_time 检索并标记 `fallback`/`event_time_message_time_fallback`；`fact_validity` 返回 `unsupported`。时间范围只能缩小检索，不能改变 viewer/群权限。

### 1.7 memory：隐私规则

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `privacy_policy_version` | `"r3"` | 写入 `raw_events.policy_version` 的策略版本 |
| `privacy_rules` | `[]` | 规则数组；无效规则会被忽略并打印日志 |

单条规则：

```json
{
    "id": "phone",
    "type": "pattern",
    "value": "\\b1\\d{10}\\b",
    "action": "redact"
}
```

| 字段 | 说明 |
|------|------|
| `id` | 规则编号；缺省为 `rule-<index>` |
| `type` | `exact` / `keyword` / `pattern` |
| `value` | 匹配内容；`pattern` 使用正则 |
| `action` | `redact`（替换）或 `block`（整条丢弃，只留审计行） |

> `blocked` 消息默认完全不回复，不写 `chat_history`，不进入蒸馏/摘要/claim/prompt。规则只能识别已配置的 exact/keyword/pattern，不宣称语义级隐私识别。

### 1.8 time

| 字段 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `night_start` | `"22:00"` | `HH:mm` | 夜晚开始，支持跨零点 |
| `night_end` | `"07:00"` | `HH:mm` | 夜晚结束 |
| `blush_lasting` | `30` | 1–3600 秒 | 脸红退火时长 |
| `emotion_lasting` | `15` | 1–3600 秒 | 表情退火时长 |
| `distance_lasting` | `15` | 1–3600 秒 | 距离退火时长 |

### 1.9 qq：连接与回复策略

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `enabled` | `false` | 是否启用 QQ 渠道 |
| `ws_url` | `ws://127.0.0.1:3001` | SnowLuma / OneBot v11 WS 地址 |
| `access_token` | 空 | OneBot 鉴权 token；不要提交到仓库 |
| `reply.private` | `"master"` | `none` / `master` / `allowlist` / `all` |
| `reply.private_allow` | `[]` | `allowlist` 模式下的 QQ 号列表；master 始终允许 |
| `reply.group` | `"none"` | `none` / `mention` / `all` |
| `reply.group_allow` | `[]` | 允许处理的群号；为空表示不限制 |
| `reply.master_any_group` | `false` | 是否允许 master 在非白名单群触发回复 |

> 回复资格与记录资格独立。`group: mention` 下未 @ 机器人的群消息只记录（若记录资格允许），不进入大脑。

### 1.10 qq：群记忆与成员关系

| 字段 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `group_memory_enabled` | `false` | bool | 群级长期记忆总开关 |
| `group_memory_allow` | `[]` | 群号数组 | 允许进入群记忆的群 |
| `group_memory_mode` | `{}` | map | 群号 → `strict_whitelist` / `group_authorized`；缺省 strict |
| `group_sender_allowlist` | `{}` | map | 群号 → 允许“记录+回复”的成员 QQ 列表 |
| `group_retention_days` | `30` | 1–3650 | 群记忆保留期；清理逻辑在 PR-5 接入 |
| `membership_refresh_minutes` | `30` | 0–1440 | 成员快照刷新间隔；`0` 关闭定时刷新 |
| `membership_max_age_minutes` | `1440` | 1–10080 | 快照超过该时长视为过期；跨群检索 fail closed |

群模式语义：

- `strict_whitelist`：只有 owner 与 `group_sender_allowlist` 成员有记录资格。
- `group_authorized`：群级 opt-in 后，非白名单成员的消息可进入群记忆，但**不获得回复资格**，不写 `chat_history`。
- `group_sender_allowlist` 决定哪些成员具备记录/回复资格；`group_authorized` 下非名单成员仍可被记录，但不回复。

示例：

```json
"group_memory_enabled": true,
"group_memory_allow": [123456789],
"group_memory_mode": {
    "123456789": "group_authorized"
},
"group_sender_allowlist": {
    "123456789": [20001]
},
"group_retention_days": 30,
"membership_refresh_minutes": 30,
"membership_max_age_minutes": 1440
```

### 1.11 tts

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `mode` | `"api"` | `api`（GPT-SoVITS）或 `mock`（模拟，不请求网络） |
| `gpt_sovits_root_path` | 空 | GPT-SoVITS 根目录 |
| `python_path` | 空 | 启动 API 服务使用的 python |
| `gpt_weights_path` | 空 | GPT 模型 `.ckpt` |
| `sovits_weights_path` | 空 | SoVITS 模型 `.pth` |
| `gpt_model_dir` | 空 | GPT 模型目录 |
| `sovits_model_dir` | 空 | SoVITS 模型目录 |
| `ref_audio_path` | 空 | 默认参考音频；为空时按情绪映射选择 |
| `prompt_text` | 空 | 参考音频对应文本 |
| `prompt_lang` | `"ja"` | 参考音频语言 |

参考音频与情绪映射由 `TTSReferenceManager` 读取 `tts_map.json`；具体资源要求见 [README](../README.md#tts-语音系统说明)。

---

## 2. identity.json

不存在时使用默认配置，桌宠行为不变；建议手动创建。

```json
{
  "identity": {
    "self": { "qq": ["10001"] },
    "master": { "qq": ["20001"] }
  },
  "memory": {
    "blind_zones": [
      { "channel": "qq_group", "session_id": "123456789" }
    ]
  },
  "appearance": {
    "follow_external_mood": true
  },
  "proactive": {
    "enabled": false,
    "daily_limit": 3,
    "cooldown_seconds": 900,
    "idle_seconds": 1800,
    "quiet_hours": { "start": "23:00", "end": "08:00" },
    "targets": [
      { "channel": "pet", "session_id": "default" }
    ],
    "schedule": [
      { "at": "07:30", "type": "fixed" }
    ]
  }
}
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `identity.self.qq` | `[]` | 机器人自己的 QQ；用于丢弃自触发/回声消息 |
| `identity.master.qq` | `[]` | 主人 QQ 列表；全部归一为 `subject='user'` / `person_id='owner'` |
| `memory.blind_zones` | `[]` | 不入长期记忆的渠道/会话；元素为 `{channel, session_id}` |
| `appearance.follow_external_mood` | `true` | D1：QQ 等外部渠道情绪是否驱动桌宠立绘 |
| `proactive.enabled` | `false` | Phase 2 主动对话预留；当前只解析不调度 |
| `proactive.daily_limit` | `3` | 每日主动上限（预留） |
| `proactive.cooldown_seconds` | `900` | 两次主动最小间隔（预留） |
| `proactive.idle_seconds` | `1800` | 闲聊冷却（预留） |
| `proactive.quiet_hours` | `23:00`–`08:00` | 免打扰时段（预留） |
| `proactive.targets` | `[pet/default]` | 主动投递目标（预留） |
| `proactive.schedule` | `[]` | 定时项：`at` + `type=fixed/random`（预留） |

> `ProactiveConfig` 当前不会自动发起对话；Phase 2 接入 `ChatPlanner` 后才生效。

---

## 3. 常见配置组合

### 3.1 只让主人私聊、群聊 @ 才回复

```json
"reply": {
    "private": "master",
    "group": "mention",
    "group_allow": [123456789]
}
```

### 3.2 群授权记录，但只有指定成员可回复

```json
"group_memory_enabled": true,
"group_memory_allow": [123456789],
"group_memory_mode": { "123456789": "group_authorized" },
"group_sender_allowlist": { "123456789": [20001] }
```

非 `20001` 的群成员：可进入群记忆（若合规），但不回复、不写 `chat_history`。

### 3.3 关闭长期记忆，保留短期上下文

```json
"memory": {
    "short_context_enabled": true,
    "long_term_memory_enabled": false
}
```

`chat_history` 仍可写；`raw_events` / interactions / claims 不写，不触发蒸馏。

---

## 4. 排查

- 配置解析失败：日志 `[ConfigManager]配置文件 JSON 损坏，保留原文件，不自动覆盖`。检查注释、尾随逗号、引号。
- 身份未生效：检查 `identity.json` 的 `self/master` QQ 是否为字符串；启动日志应出现 `identity.json 加载完成: self N 个 / master N 个`。
- 群成员快照不更新：检查 `qq.enabled`、`ws_url`、`membership_refresh_minutes`，以及日志中的 `[Membership]` 快照成功/失败。
- 群记忆不生效：检查 `group_memory_enabled`、`group_memory_allow`、群模式与 sender allowlist；注意 `group_authorized` 只放宽记录，不放宽回复。
