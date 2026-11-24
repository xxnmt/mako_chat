# B 阶段连续运行验收（B5）观测手册

> 更新：2026-11-23 · 对应实现见 [B 阶段施工单](B_STAGE_TASKS.md)
> 用途：实机验收时逐项打勾、记录证据。工程验收部分已自动化；本手册只写"要跑什么、看什么、什么算过"。

## 0. 前置

1. 备份运行库：`app_data/memory/QianDaoMoZi_memory.db`（含 `-wal/-shm`）。
2. 启动应用一次，确认日志出现 `数据库初始化完成,结构版本为: 25`。
3. 记录本次日志文件名：`app_data/logs/logs_<时间戳>.txt`（下文 `$LOG`）。
4. 观测入口：
   - `$LOG` 关键字：`[Attention]`、`[Concern]`、`[Reflection]`、`[Reminder]`、`[Delivery]`、`[Share]`
   - DB（只读副本）：`private_reflection`、`reflection_change`、`concern_resolution`、`reflection_contact`、`delivery_ledger`、`attention_event`、`attention_state`、`chat_history`

## 1. 工程验收（已自动化，`ctest`）

| 套件 | 覆盖 |
|---|---|
| `tst_concern` | 无检索命中的 prior、前台了结、重复了结拒绝 |
| `tst_memorymanager` | v23–v25 迁移、到期关注领取/keep/resolve/contact、匹配精度（DF）、主动联系重试一次、主动消息写历史、分享去重、注意力事件状态机、恢复事件、活动来源/产物、删除来源阻止分享 |
| `tst_memoryagent` | 回顾仲裁、到期关注路由、重启恢复标记 `restart_recovery`、前后台读回 |
| `tst_qqdelivery` | QQ 串行发送与回执 |

通过标准：除环境性 `tst_runtimelog`（QProcess 管道在宿主沙箱不可用）外全绿。

## 2. 实机场景

### A · 关注生命周期（答案了结 + 跨天不重复）

1. 发生一件她会在意的小事（例：你在等一份外卖），正常聊完。
2. 下次对话里直接给出结果（"已经吃完了，很好吃"）。
3. 观测：`$LOG` 出现 `[Concern] provided refs=[...]`；若模型遵守契约，出现 `[Concern] resolved ... ref=... kind=answer`。
4. 核对 DB：
   ```sql
   select turn_id,state,resolved_by,resolved_kind from private_reflection where turn_id=<关注头>;
   select * from concern_resolution order by id desc limit 3;
   ```
   期望：`state='resolved'`、`concern_resolution` 恰 1 行、之后 `matchedConcerns` 不再返回它。
5. 跨天/换渠道/重启后再聊该话题，**不得**再生成同一疑问。

**注意**：只验证"数据层是否了结"，不把"她是否重复问"当唯一判据；若模型没写了结，记录为契约遵守问题，不判实现失败。

### B · 到期关注 → 有理由的主动联系

1. 留下一个带明确下一步的关注（例："他明天体检，记得问他结果"），`next_check_at` 在几小时后。
2. 到点前后一段时间不要发消息。
3. 观测：`$LOG` 出现 `[Concern] reviewing`（kind=concern），随后 `[Concern] committed ... contact now` 或 `keep`。
4. 若选择 `now`：核对
   ```sql
   select timing,outcome,attempts,last_outcome from reflection_contact where turn_id=<头>;
   select kind,ref,state from delivery_ledger where kind='contact' order by id desc limit 3;
   select id,turn_kind,substr(raw_reply,1,80) from chat_history order by id desc limit 3; -- 应有 proactive 轮
   ```
   期望：`timing=now`、`outcome` 走到 `handed_off/protocol_accepted`；无重复行；proactive 历史行存在。
5. 负向：无新证据、未到约定时间的日子，24h 内不得出现 `now` 投递。

### C · 新证据唤醒

1. 关注 X 处于 `next_check_at` 较远的状态。
2. 从**另一渠道**或群聊发一条与 X 相关的消息（不需要直接回答）。
3. 观测：60s 内 `[Attention] backlog` 出现 `due_wake`；下一次回顾上下文里能看到该消息（`since`）。
4. 期望：`keep` 且 `next_check_at` 顺延，或 `contact` 且理由引用新证据；不得无理由 `now`。

### D · 重启恢复

1. 在回顾进行中直接结束进程（任务管理器）。
2. 重启。
3. 观测：`$LOG` 出现 `[Attention] recovered turn=...`；该轮 `chat_history.turn_state='interrupted'`、`turn_detail='restart_recovery'`。
4. 期望：不会对同一来源重复决策；`attention_event` 中 recovery 事件为 `done`。
5. 若之前有 `dispatching` 的联系/提醒：`rejected/unknown` 有明确归宿；`unknown` 不重发，`rejected` 最多补一次。

### E · 活动/作品连续

1. 触发一个写作活动（自然聊天里同意/她自己产生兴趣），产生草稿。
2. 重启；再触发一次回顾，确认读回草稿与下一步。
3. 完成后要求分享。
4. 观测：`thought_result` 含 `source_turn/goal_kind/artifact_ref/updated_at`；`share:%id:%ver` 回执写入 `delivery_ledger(kind='share')` 与 `thought_result.share_outcome`。
5. 下次问"你写的那首诗呢"：应能引用作品与分享结果，而不是空口复述。

### F · 打断、取消与放弃

1. 活动/回顾进行中发一条消息 → 观测 `[Attention] switch reason=incoming_message`，活动保存 `paused_reason`；恢复后草稿仍在。
2. 取消活动（`abandon`）→ 迟到回调 `finishThoughtReview` 必须失败；`canShareThought=false`。
3. 删除活动来源轮 → `nextThought` 为空、不可分享、迟到回调不复活。

## 3. 7 天观察表

| 指标 | 采样 | 目标 |
|---|---|---|
| 到期关注处理率（48h 内 keep/revise/resolve/contact） | 每日 | 100% |
| `now` 带理由比例 | 每条 | 100% |
| 无理由 `now` 条数 | 每日 | 0 |
| 已了结话题重复追问 | 人工判读 | 0（数据层必须为 0） |
| 主动投递回执率 | 每条 | `protocol_accepted` 或明确 `rejected/unknown` |
| 重复发送 | 每日 | 0 |
| 重启恢复 | 每次重启 | 无重复决策/无丢失 |

## 4. 失败判定与回滚

- **实现缺陷**：DB 状态与日志不符（如 `now` 已投递但 `reflection_contact` 无记录；`resolved` 后仍被注入）。
- **模型契约问题**：`provided` 出现但模型从不输出 `concern_updates`/`concern_action`（调提示词，不判实现失败）。
- 回滚：整库备份恢复；或最小回滚：
  ```sql
  update private_reflection set state='consider',resolved_by=null,resolved_kind=null where turn_id in (...);
  delete from concern_resolution where prior_turn in (...);
  ```

## 5. 改进批次的观测点（M5）

- **日志会话头**：每个新日志文件第一条应为
  `[RuntimeLog] session app=Windows_AI version=0.3 pid=... qt=6.11.1 build=... exe_dir=... config=... config_hash=<12位>`。
  配置改动后 `config_hash` 会变，可用于判断"这次行为和上次不同是不是配置漂移"。
- **能力清单**：前台每轮有一条 `【你当前被授予的能力（服务端核验，只读）…】` 的只读 JSON；若她声称了清单之外的能力（例如没开主动联系却说"我待会找你"），记一次契约偏差。
- **重要性召回**：检索证据 JSON 中出现 `importance`；同一个重要事件（承诺、里程碑）在相关提问中应更靠前召回。可在 DB 里把某条 `episodic_memory.importance` 调高做对照（只读观测，不建议在正式库改数据）。
- **主动联系日限额**：`contact_daily_limit`（默认 3）生效时，超限日志为 `[Contact] daily_limit reachable`；`delivery_ledger` 中当日 `kind='contact'` 行数不超过限额。
- **关注默认间隔/保留期**：新关注未指定时间时按 `concern_default_check_minutes`（默认 720）排期；超过 `concern_retention_days`（默认 14）的旧关注不再按时间唤醒，但仍可被新证据命中。
- **分享/联系重试**：`rejected` 最多补一次；`unknown` 不重发；均以 `delivery_ledger` 为准。

## 5.1 可靠性补丁的观测点

- **普通回复投递**：每句 `delivery_ledger(kind='reply', ref='reply:<turn>:<index>')`；断线时在途为 `unknown`、排队为 `rejected`；`rejected` 最多补一次，`unknown` 永不重发。核对：
  ```sql
  select ref,state,attempts,last_error from delivery_ledger where kind='reply' order by id desc limit 10;
  ```
  日志关键字：`discard queued sends`、`发送失败 retcode`、`[QQ]回执`。
- **到期修订流水**：`select turn,action,note_before,note_after,next_check_at from concern_revision order by id desc limit 10;`（旧想法不再被静默覆盖）。
- **启动残留恢复**：启动日志出现 `[Concern] stale_reflections_recovered=N` 或 `[Attention] stale_reflections_recovered=N`。
- **能力清单当前活动**：清单 JSON 中出现 `current_activity{ref,status,progress,next_step}`，与 `attention_state.current_activity` 一致。
- **日志噪声**：空 `[Attention] backlog` 与 `idle skip reason=no_idle` 只在 DEBUG 出现；INFO 只在有事件/配置问题时出现。

## 6. 已知边界（不作为失败）

- 多活动并行选择未实现（沿用单活动，ROADMAP 已列为后续）。
- "先决定是否阅读消息"只留了 attention 事件与状态记录，不做元数据优先的门控（重构方案 §1.2 明确不扩大）。
- `unknown` 投递不重发（避免重复），只记录。
- 主动消息历史在 `handed_off` 时写入；若随后协议层 `rejected`，由补发与 `(kind,ref)` 去重保持一致。
