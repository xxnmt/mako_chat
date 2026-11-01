# Memory Recall Tool：主 Conversation Agent 自主检索

> 状态：已实现（PR-6），默认 `agent_mode=tool`
> 关联：[ARCHITECTURE.md](ARCHITECTURE.md)、[CONFIG_REFERENCE.md](CONFIG_REFERENCE.md)、[MEMORY_V5_R3_BASELINE.md](MEMORY_V5_R3_BASELINE.md)

## 1. 目标

把“要不要回忆、回忆什么”从关键词门控和独立 Planner 交回主 Conversation Agent；程序只保留能力门控、权限、时间解析和确定性检索。

```text
用户消息 + 最近对话
        │
        ▼
主 Conversation Agent（同一上下文）
        │
   ┌────┴─────┐
   │          │
不调用      调用 memory_recall
   │          │
最终 JSON   MemoryTimeResolver
              │
           MemoryExecutor
              │
           role=tool 结果
              │
           同一 Agent
              │
           最终 JSON
```

## 2. 责任边界

| 角色 | 负责 |
|---|---|
| 主 Conversation Agent | 是否调用、拆 needs、理解纠正/指代、最终回复 |
| `canExposeTools()` | 功能开关、QQ 渠道、`turn_kind=reply`、pet 排除 |
| `MemoryRecallTool` | 参数校验、调用 Resolver/Executor、返回证据 |
| `MemoryTimeResolver` | 每个 need 独立解析时间 |
| `MemoryExecutor` | 只读检索、去重、预算、per-need 状态 |
| 服务端 `MemoryAgentScope` | viewer/owner/channel/session/allowedGroups/membership |
| `MemoryOutputGuard` | 最终输出保护 |

程序不读用户文本判断“是不是记忆问题”；`agent_trigger` 只属于 legacy 路径。

## 3. 工具契约

```json
{
  "needs": [
    {
      "id": "need-1",
      "query": "用户表达过的想法、愿望或计划",
      "kinds": ["recent", "summary", "claim"],
      "time": {"expression": "昨天下午", "target": "message_time"}
    }
  ]
}
```

- 最多 4 个 need；每个 need 最多 8 条，总计最多 16 条。
- 只接受 `id/query/kinds/time`；`time` 只接受 `expression/target`。
- 不接受 `from/to`、viewer/owner/group_id/session_id/allowed_groups。
- `source_hint` 只在旧协议兼容层接受，不进入主工具 schema。
- `kinds` 只允许 `recent/summary/claim`；`target` 只允许 `message_time/event_time/fact_validity`。

## 4. 状态语义

need：

| 状态 | 含义 |
|---|---|
| `success` | 取得有效授权证据 |
| `empty` | 查询完成但无结果 |
| `unresolved` | 时间表达无法解析 |
| `unsupported` | 当前不支持的时间目标/检索类型 |
| `denied` | 访问范围不允许 |
| `error` | 查询异常 |

整体：

| 状态 | 含义 |
|---|---|
| `complete` | 所有 need 正常结束（包括 empty） |
| `partial` | 存在 unresolved/unsupported/denied/error/timeout/fallback/truncated |
| `empty` | 所有 need 正常结束但无证据 |
| `failed` | 执行器不可用且无安全结果 |

`need-1=empty`、`need-2=success` 为 `complete`，同时 `has_empty=true`；不能伪装成两个都成功。

## 5. Inline / Spill

| 资料 | 进入方式 |
|---|---|
| 角色人设、稳定用户档案、必要关系 | Inline 自动注入 |
| RecentContext | Inline，严格预算 |
| episodic | Spill，`memory_recall` 按需 |
| interaction summary | Spill，`memory_recall` 按需 |
| claim | Spill，`memory_recall` 按需 |
| 长期 summary | Spill，`memory_recall` 按需 |
| pet 渠道 | 既有 automatic top-k，不进入本工具 |

## 6. 协议

- 默认 `agent_protocol=native`：`tools` + `tool_choice=auto` + `response_format=json_object`；工具结果用 `role=tool` + `tool_call_id` 回放。
- `json_envelope`：主模型输出 `{"action":"recall","needs":[...]}`，执行后同一模型再输出最终 JSON。
- native 请求被 provider 以 400/422 拒绝时自动降级 envelope，并记录一次 fallback。
- 最多 `agent_max_rounds` 轮；每轮结果保留，超时不撤销已取得证据。

## 7. 配置

| key | 默认 | 说明 |
|---|---|---|
| `memory.agent_enabled` | `true` | 开关 |
| `memory.agent_mode` | `tool` | `tool` / `legacy` |
| `memory.agent_protocol` | `native` | `native` / `json_envelope` |
| `memory.agent_timeout_ms` | `6000` | 检索预算 |
| `memory.agent_max_rounds` | `2` | 工具轮数 |
| `memory.agent_max_results` | `8` | 单 need 结果上限 |
| `memory.agent_plan_v2` | `true` | 仅 legacy |
| `memory.agent_trigger` | `memory_question` | 仅 legacy |

## 8. 回滚与灰度

- `agent_mode=legacy` 恢复旧关键词门控 + 独立 Planner；不需要数据库迁移。
- `agent_mode=tool` 是默认；灰度期间保留 legacy 代码与测试。
- 不在本阶段引入向量库、schema 变更、Router/Worker/Ticket、Watchdog 或主动巡检。

## 9. Tool Calling 兼容性实测

2026-11-01 用当前 `deepseek-v4-flash` 配置做了最小探测：

| 请求 | 结果 |
|---|---|
| `tools` + `tool_choice=auto` + `response_format=json_object` | `finish_reason=tool_calls`，返回 `memory_recall` 调用 |
| assistant `tool_calls` + `role=tool` + `tool_call_id` 回放 | 第二轮 `finish_reason=stop`，返回最终 JSON |
| 工具结果后再追加一条 system 说明 | 仍能正常返回最终 JSON |
| 普通闲聊（工具可用） | 不返回 `tool_calls`，直接最终 JSON |

结论：`native` 作为默认协议；`json_envelope` 仍保留为 provider 400/422 或未来模型差异的兜底。

## 10. 验收集

| 输入 | 期望 |
|---|---|
| “今天心情不错” | 不调用工具 |
| “刚刚吃了一个可颂” | 不调用工具 |
| “昨天下午我有没有对你说过什么愿望？” | 调用工具 |
| “上周三和昨天晚上分别聊了什么？” | 两个 needs |
| “不对，那个是周三，不是昨天” | 结合上下文修正 |
| “上个月某个时候” | unresolved，不猜测 |
| need-1 empty、need-2 success | per-need 正确，整体 complete + has_empty |
| 非 Owner 群里问私聊 | 拒绝/不可访问 |
| 工具参数伪造 viewer/group/scope | 服务端拒绝 |
| pet 渠道 | 不进入本工具 |
