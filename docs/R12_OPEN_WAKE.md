# R12 自主认知入口实施状态

基线：`c28d23dcd03bc7234bb6397f459f2563f8a7886b`（R12 开工时 HEAD），数据库 schema **v30**。
本文只记录已实施/已确认事项；未实现状态不得写成已完成。

## 切片状态

| 切片 | 状态 | 说明 |
|---|---|---|
| R12-0 诊断/契约/退避/日志/快照统计 | **已完成（源码+测试已提交）** | 无 schema 变更、无发送行为变化；运行中的旧进程需重启后生效 |
| R12-1a OpenWake 持久化 + 共享调度 + 最小快照修复 | 待确认后实施 | 需 v31；活动存储决策已确认 A |
| R12-1b 统一活动管理（internal_activity） | 待实施 | 统一生命周期/领取/CAS/恢复/分享/快照；存量活动兼容适配 |
| R12-2 自主行动契约 + 真实投递 | 待实施 | 新增 commitOpenWakeContactAction；Evidence Purpose；commit/dispatch 复验 |
| R12-3 认知快照连续性优化 | 待实施 | 跨入口一致性、去重、来源追踪、预算优化 |

## R12-0 已落地

- `PrivateReview::wake` 返回类型化 `WakeResult`（started/kind/reason/sourceRef/version/nextEligibleAt），coordinator 不再用临时信号推断是否启动；`[Wake] tick`、`[Review] attempt/result`、`[Contact] proposal` 结构化日志。
- 回顾契约统一：reflection/concern 共用规则文本，删除新提案“必须有新主人证据/basis 才能 now”的旧约束；补充服务端 `time_context`（now_local/now_utc/source_reported_at/last_owner_interaction_at/last_confirmed_outbound_at/quiet_hours）；legacy basis 读取保留。
- Reflection 失败退避：120/300/900 秒，默认第 4 次连续失败 suspended（86400 秒）；用户插话/关停的主动让出不累计失败；失败计数当前为进程内临时态，跨重启可靠计数留给 v31。
- 日志降噪：`[Attention] backlog` 只在 pending/oldest 非空时 INFO（done-only DEBUG）；`expired_by_retention` 聚合为一次 claim 一条 INFO + 逐 turn DEBUG + 进程内去重，不改任何事实/关注。
- 快照每域统计：queried/eligible/serialized/omitted、bytes_before/bytes_after、裁剪 notes；日志 DEBUG 每域 + INFO 摘要。**本片未改 1200 预算、未移除静态说明、未改裁剪顺序**（按确认放到 R12-1a）。

## 已确认的设计约束（待后续切片实施）

- 活动存储选独立 `internal_activity`，禁止伪造 chat_history 行/user turn；统一活动访问接口，存量活动兼容；单 active/paused 跨两种存储统一判断。
- OpenWake 共享 `thought_review_log`，新增 source_kind/source_ref/run_id；独立每日上限默认 12、共享预算占用约 30%、到期后最多 3 次其他普通回顾优先；`daily_limit=0` 禁止所有后台认知；提醒链独立到期不受公平性影响。所有配额是上限，不是必须完成次数。
- `open_wake.enabled=false`、`allow_contact=false`；首次延迟 15 分钟、默认后续 30 分钟、模型可在 5–1440 分钟范围内提议；不强制主动联系。
- R12-1a 完成说明去重、非空域最小保留、OpenWake 内部记录读取；R12-3 再做跨入口一致性/去重/来源追踪/预算优化。
- 后台通用 memory_recall tool loop 后置；本轮复用现有上下文与有限 consult。
- R12-0 无 schema；R12-1a v31 一次迁移，包含持久化失败计数/退避、OpenWake 表、ledger 列、活动表（若需要）。
- 禁止负数 ID/伪造 turn/无条件 concernTurn=0 绕过来源校验；异步回调校验 kind/ref/version/runId/scope/epoch；服务端生成 semantic_status；unknown 不重发、不写成功历史。
