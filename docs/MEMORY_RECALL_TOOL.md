# Memory Recall Tool：数据源、授权、概览与时间语义

> 状态：PR-7A / PR-7B / PR-7C / PR-8 / PR-9 / PR-11 已实现；PR-10 legacy 保留为回滚
> 关联：[ARCHITECTURE.md](ARCHITECTURE.md)、[CONFIG_REFERENCE.md](CONFIG_REFERENCE.md)

> 本文只负责既有 recall/read 工具契约。personalEvidence/currentUnderstanding 的内部证据准备路径见 [架构](ARCHITECTURE.md)，未来重构任务见 [路线图](ROADMAP.md)。

## 1. 目标

主 Conversation Agent 自主决定是否调用记忆工具；服务端负责数据源范围、逐条披露、时间计算、覆盖率和审计。

```text
用户消息 + 最近对话
        │
        ▼
主 Conversation Agent
        ├─ lookup：定点召回，top-k + per-need 状态
        └─ overview：完整时间段扫描 + 覆盖率 + 时间桶
                         │
                    memory_read(bucket)
                         │
                    同一主模型最终回复
```

## 2. 数据源

| kind | 数据源 |
|---|---|
| `group_recent` | 当前群/授权群近期消息 |
| `interaction_summary` | `interactions.safe_summary` |
| `claim` | `memory_claims`，经 `MemoryAccessPolicy` |
| `episodic` | `episodic_memory` |
| `long_term_summary` | `long_term_summary` |
| `chat_history` | `chat_history` 原始对话 |

为兼容 PR-6，`recent` / `summary` 作为旧别名继续接受；新调用使用新名称。
`profile` / `relationship` 继续 Inline，不进入检索。

## 3. 授权矩阵

| 数据源 | Owner 私聊 | Owner 群聊 | 非 Owner 私聊 | 非 Owner 群聊 |
|---|---|---|---|---|
| `group_recent` | 授权群 | 当前群 | 仅共同群、viewer 范围 | 当前群权限 |
| `interaction_summary` | 授权群 | 当前群 | **不直接开放群摘要** | 当前群权限 |
| `claim` | AccessPolicy | AccessPolicy | AccessPolicy | AccessPolicy |
| `episodic` | pet/private 需开关 | 仅 public/channel 当前群 | 禁止 | 禁止 |
| `long_term_summary` | pet/private 需开关 | 仅当前群/渠道 | 禁止 | 禁止 |
| `chat_history` | pet/private 需开关 | 当前群 | 禁止 | 禁止 |

`memory.owner_cross_channel_private=false` 默认关闭。开启后只对 Owner QQ 私聊生效。
`episodic.subject` 只是关联主体，不是逐条隐私所有者；记录仍要检查 `origin/visibility/session/source_ids`。

## 4. 时间语义

| 数据源 | `message_time` | `event_time` | `fact_validity` |
|---|---|---|---|
| `group_recent` | `occurred_at` | fallback/不支持 | 不支持 |
| `interaction_summary` | interaction 时间区间重叠 | 不支持 | 不支持 |
| `episodic` | 通过 `source_ids` 回溯；否则 content-only | 使用 `event_time` | 不支持 |
| `long_term_summary` | 通过 `covered_turn_end_id/source_ids` 回溯；不能用 `created_at` | 不支持 | 不支持 |
| `claim` | source session/event | 不支持 | `unsupported` |
| `chat_history` | turn timestamp | 不支持 | 不支持 |

禁止：

```text
episodic.event_time == message_time
long_term_summary.created_at == message_time
```

## 5. 工具契约

```json
{
  "mode": "lookup",
  "needs": [
    {
      "id": "need-1",
      "query": "七夕约定",
      "kinds": ["episodic", "long_term_summary"],
      "time": {
        "expression": "七夕",
        "target": "message_time",
        "frame": {
          "anchor": {"type": "festival", "name": "七夕"},
          "occurrence": {"type": "most_recent_past"},
          "span": {"type": "day"}
        }
      }
    }
  ]
}
```

约束：

- `lookup`：1–4 个 needs；`overview`：必须且只能有 1 个 need；
- 每个 need 最多 8 条，总计最多 16 条；
- `mode=lookup|overview`；
- 不接受 `from/to`、viewer/owner/group/session/allowed_groups；
- **只要 need 带 `time`，tool 路径就必须同时提供 `time.frame`**；只有 `expression` 时返回 `time_frame_required`，不再默认回退旧 Resolver；
- `frame` 只接受受控字段，时区由服务端提供；
- `absolute_date` 由模型复制、服务端校验，模型不计算日期；
- `legacy` 路径仍可使用旧 Resolver，但那不是普通 tool 路径。

### 5.1 JSON Envelope 回退

`agent_protocol=json_envelope` 与 Native 使用同一套参数语义：

```json
{
  "action": "recall",
  "mode": "lookup|overview",
  "overview": {"bucket_minutes": 180, "max_buckets": 8},
  "needs": [
    {
      "id": "need-1",
      "query": "脚踝",
      "time": {
        "expression": "两天前",
        "target": "message_time",
        "frame": {
          "anchor": {"type": "relative_day", "offset": -2},
          "span": {"type": "day"}
        }
      }
    }
  ]
}
```

`MemoryRecallPlan::fromEnvelope` 会保留 `mode` / `overview` / `needs[].time.frame`；回退协议不会退回默认 lookup，也不会丢掉结构化时间。

## 6. Overview

```json
{
  "mode": "overview",
  "needs": [{
    "id": "overview-1",
    "query": "今天群聊",
    "time": {
      "expression": "今天",
      "frame": {
        "anchor": {"type": "relative_day", "offset": 0},
        "span": {"type": "day"}
      }
    }
  }],
  "overview": {"bucket_minutes": 180, "max_buckets": 8}
}
```

服务端扫描时间范围内全部 ready interaction summaries，返回：

- `coverage.total_interactions`；
- `ready_summaries`；
- `scanned` / `aggregated` / `exposed`；
- `missing_summaries` / `missing_ranges` / `empty_buckets`；
- `scan_complete` / `aggregation_complete` / `exposure_complete`；
- `buckets[]`，每桶带 `representative_summaries` 和 `handles`。

`handles` 对应桶内完整摘要集合，`memory_read` 可展开；representative 只是直接交给模型的子集。
`bucket_minutes` 默认 180，`max_buckets` 默认 8，由服务端限幅；模型不决定资源预算。

`max_buckets` 截断桶数时，服务端同步拉长实际桶跨度，保证最后一个桶覆盖到 `coverage.to`。
例：一周 / 8 桶时每桶约 21 小时，而不是 8 个固定 3 小时桶后把剩余记录塞进最后一桶。

Overview 只接受 **1 个 need**；多个 need 返回 `invalid_overview_plan`，不会静默只执行第一个。
Owner 私聊发起群 Overview 时必须有且只有一个授权群；多个授权群返回 `overview_group_ambiguous`，不会默认选第一个。

## 7. 状态与审计

need：`success / empty / unresolved / unsupported / denied / error / fallback`。
整体：`complete / partial / empty / failed`。

`need-1=empty`、`need-2=success` 为 `complete`，同时 `has_empty=true`。

每个 need 输出：

```text
status / target / evidence_state / time_certainty
expression_length / expression_hash
frame_present / anchor_type / frame_offset / span_type
resolver_type / resolver_status
access_scope_status
resolved_from / resolved_to / time_constraint_applied
source_kinds / scanned_count / result_count / latency_ms
```

`resolver_type` 区分 `time_frame` / `legacy` / `none`；`resolver_status` 区分 `ok` / `unresolved` / `frame_required` / `unsupported`。

日志只记录 need id、状态、时间摘要、解析器路径、范围、来源、数量、耗时，不记录 query 原文。时间表达式只记录长度和 hash。

工具结果带 `scope`：

```json
"scope": {
  "access_scope": "current_group",
  "other_channels_visible": false
}
```

`other_channels_visible=false` 时，用户问“另一个地方/QQ 私聊”只能说明当前渠道看不到，不能把范围受限说成全局没有，也不能确认私人记录是否存在。

## 8. TimeFrame

```json
{
  "anchor": {"type": "festival", "name": "七夕"},
  "occurrence": {"type": "year_offset", "value": -1},
  "span": {"type": "week_containing"},
  "period": null
}
```

支持：

- `relative_day` + `offset`；
- `relative_week` + `offset` + `weekday`；
- `relative_month` + `offset`；
- `absolute_date` + `date`；
- `festival` + `name`；
- `recent_window` + `minutes`；
- `occurrence`: most_recent_past / next_future / year / year_offset；
- `span`: day / week_containing / surrounding_days(before/after) / month_containing；
- `period`: morning / afternoon / evening / night。

公共节日在 `calendar/festivals.json`；新增节日不改 Resolver 代码。当前数据支持 2024–2026，超出返回 `unresolved`。用户纪念日不进入公共节日文件。

## 9. 配置

| key | 默认 | 说明 |
|---|---|---|
| `memory.agent_enabled` | `true` | 开关 |
| `memory.agent_mode` | `tool` | `tool` / `legacy` |
| `memory.agent_protocol` | `native` | `native` / `json_envelope` |
| `memory.owner_cross_channel_private` | `false` | 仅 Owner 私聊读取自己的 pet/private |
| `memory.agent_timeout_ms` | `6000` | 检索预算 |
| `memory.agent_max_rounds` | `2` | 工具轮数 |
| `memory.agent_max_results` | `8` | 单 need 结果上限 |
| `memory.agent_plan_v2` | `true` | 仅 legacy |
| `memory.agent_trigger` | `memory_question` | 仅 legacy |

## 10. 验收集

| 输入 | 期望 |
|---|---|
| 普通闲聊/“刚刚吃了一个可颂” | 不调用工具 |
| “七夕约定” | 调用；Owner 私聊开关开启时返回 episodic/long term |
| Owner 群聊问 pet/private | 不返回、不泄露存在性 |
| “总结今天群里的消息” | `mode=overview`，覆盖 146 条，分桶 |
| “上周三晚上” | TimeFrame relative_week |
| “去年七夕那周” | festival + year_offset + week_containing |
| “某个时候” | unresolved，不猜测 |
| bucket handle | `memory_read` 能展开桶内完整摘要 |
| 工具参数伪造权限 | 拒绝 |

## 单轮补查预算（阶段 A）

前台每轮最多三次不同补查，内部事项历史、公开工具及事项 consult 共用预算。重复请求不增加可用资料。时间检索审计关联本轮与解析范围；“现在”的时间窗若不包含请求时刻，不按错误区间返回命中。后台观察的重试预算独立。
