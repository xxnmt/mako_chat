# R12 自主认知入口实施状态

基线：`c28d23dcd03bc7234bb6397f459f2563f8a7886b`（R12 开工时 HEAD），数据库 schema 已由 R12-1a 升到 **v31**。
本文只记录已实施/已确认事项；未实现状态不得写成已完成。

## 切片状态

| 切片 | 状态 | 说明 |
|---|---|---|
| R12-0 诊断/契约/退避/日志/快照统计 | **已完成（源码+测试已提交）** | 无 schema、无发送行为变化 |
| R12-1a OpenWake 持久化 + 共享调度 + 最小快照 | **已完成（源码+测试已提交）** | v31；OpenWake 仅 rest/reflect，allow_contact=false；无活动创建/投递 |
| R12-1b 统一活动管理（internal_activity 完整生命周期） | 待实施 | 存量活动兼容适配；OpenWake 活动创建仍未启用 |
| R12-2 自主行动契约 + 真实投递 | 待实施 | commitOpenWakeContactAction；Evidence Purpose；commit/dispatch 复验 |
| R12-3 认知快照连续性优化 | 待实施 | 跨入口一致性、去重、来源追踪、进一步预算优化 |

## R12-0 已落地

- `PrivateReview::wake` 类型化 `WakeResult`；`[Wake] tick`、`[Review] attempt/result`、`[Contact] proposal` 结构化日志。
- reflection/concern 共用回顾规则，删除新提案 basis 硬要求（legacy 兼容保留）；服务端 `time_context`。
- Reflection 失败退避 120/300/900 秒、默认第 4 次 suspended；用户插话/关停不计数。
- backlog 只在 pending/oldest 非空时 INFO；`expired_by_retention` 聚合去重。
- 快照每域 queried/eligible/serialized/omitted 统计。

## R12-1a 已落地（v31）

- **v31 迁移**（单事务、幂等）：`open_wake_state`、`open_wake`、`internal_activity`、`review_failure`、`thought_review_log.source_kind/source_ref/run_id`。旧库升级、重复启动、旧数据/Outbox 完整已测。
- **持久失败/退避/暂停**覆盖 Reflection/Concern/Activity/OpenWake：连续失败按 120/300/900 退避、默认第 4 次 suspended 24h；内容哈希检测实质修订（不误判正常领取的 version 增长）；用户插话/策略拦截 countsAsFailure=false；失败行+源状态同事务，写失败回滚；暂停到期恢复计数并允许超出 24h 来源窗口重领；源消失时清 failure 并审计。
- **OpenWake**：独立槽位/运行租约/唯一 running/重启中断恢复；共享 `thought_review_log` 预算且独立每日上限、共享预算比例、取整至少 1 不突破全局；公平计数按实际成功领取持久化；到期后最多 3 次普通回顾优先（可配）；OpenWake 只 rest/reflect，rest 合法、不产生 contact/activity/outbox，不写 current_activity。
- **类型化 CognitiveOrigin**：kind/ref/id/version/runId/scope/epoch 随请求/回传/取消边界传递；所有来源校验；不匹配回调丢弃；OpenWake epoch 由 claim 的 context epoch 提供。
- **最小快照修复**：静态说明移出 JSON 数据预算；预算默认 4096（2048..8192 可配）；非空域最小 1 条保留、预算不足显式 overflow/notes；OpenWake 内部 note 以 `wake/<id>:<version>` + InternalState 进入 inferences/motivation，不进 observations；跨渠道与当前会话最近交互时间分开；群聊不读取私人状态。
- **持久失败接线**：运行时使用 MemoryManager failure 状态机，不再有进程内计数；OpenWake/Reflection/Concern/Activity 失败分类、退避和恢复可用。

## 测试与运行验证（R12-1a）

- 全量 `ctest 23/23`；`tst_memorymanager` 122 passed/1 skipped、`tst_memoryagent` 86、`tst_cognitivesnapshot` 13、`tst_configmanager` 17。
- 生产库从 v30 升级到 v31：旧数据行数不变（chat 534、reminder 3、reflection 65、ledger 137、thought_review_log 101），新表/列齐全，重复启动幂等。
- 真实运行一次（临时启用 OpenWake、allow_contact=false、max_per_day=2）：`[OpenWake] claimed` → `[Review] attempt kind=open_wake` → `[OpenWake] committed decision=reflect`；`[Outbox]=0`、无 contact；同轮 concern review 因 `truncated` 进入持久退避并记录；快照 4096 预算下 observations/internal_wakes 各 8 条、其余非空域至少 1 条。验证后恢复原配置并清理测试产生的 OpenWake 行。
- 旧二进制/new binary：已重建当前应用二进制；验证后应用已停止。

## 已确认的设计约束（后续切片实施）

- 活动存储 A：独立 `internal_activity`，禁止伪造 chat_history 行/user turn；单 active/paused 跨存储统一；R12-1b 再启用完整创建/生命周期。
- OpenWake 参数默认：enabled=false、allow_contact=false、首次 15 分钟、默认后续 30 分钟、模型范围 5–1440、每日 12、共享 30%、N=3；配额是上限不是任务。
- 后台 tool loop 后置；R12-1a 只用现有上下文/有限 consult。
- 不改变提醒链独立执行；禁止绕过证据/来源/投递可靠性。

