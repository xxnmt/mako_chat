# R12 自主认知入口实施状态

基线：`c28d23dcd03bc7234bb6397f459f2563f8a7886b`（R12 开工时 HEAD），数据库 schema 已由 R12-1a 升到 **v31**。
本文只记录已实施/已确认事项；未实现状态不得写成已完成。

## 切片状态

| 切片 | 状态 | 说明 |
|---|---|---|
| R12-0 诊断/契约/退避/日志/快照统计 | **已完成（源码+测试已提交）** | 无 schema、无发送行为变化 |
| R12-1a OpenWake 持久化 + 共享调度 + 最小快照 | **已完成（源码+测试已提交）** | v31；OpenWake 仅 rest/reflect，allow_contact=false；无活动创建/投递 |
| R12-1b 统一活动管理（internal_activity 完整生命周期） | **已完成（源码+测试已提交）** | 前台/OpenWake 新活动写 internal_activity；legacy thought 只读适配；legacy 分享保留，internal 分享仅落意愿 |
| R12-2a 通用 ActionOrigin/Proposal、Evidence Purpose、统一核验 | **已完成（源码+测试已提交）** | v32 `action_proposal`；不发送、不接 Outbox；生产库仍 v31，待 R12-2b 受控迁移 |
| R12-2b OpenWake 联系/内部活动分享提交、Outbox、回执 | **已完成（源码+测试已提交）** | v32；allow_contact 默认 false；只入 Outbox，受控测试用 mock；生产库已 v32、零新发送 |
| R12-2c 行动一致性与认知连续性修正 | **已完成（源码+测试已提交）** | v33 仅代码/测试；生产库仍 v32，迁移待独立评审 |
| R12-3a/b 统一认知上下文/状态连续性 | 待实施 | 按要求后置，不提前开始 |

## R12-1a 补充修正（已独立提交）

- OpenWake 显式 `after_minutes` 在提交边界按管理员 `min_interval_seconds/max_interval_seconds` 夹紧；记录 requested/effective，协议外值才 schema 失败；默认 min=300 秒。
- 所有真实回顾领取生成独立 runId，成功/失败回调均必须带服务端 `_origin`；删除生产路径 id/version 兼容放行，迟到/串线回调丢弃。
- Concern 保留期结束终止定时重试：清 failure、`next_check_at=0`、保留关注/事实、记录 `[Review] retry_expired`；新证据仍可命中。
- Prompt 旧 `contact.basis` 仅作为旧数据兼容说明，不再覆盖 motivation/claims/self_desire 新契约。
- OpenWake 内部记录与调度状态按 owner scope、盲区、群聊/第三方隔离；不扩大资料读取权限。

## R12-1b 已落地

- **ActivityRef**：`thought/<id>`（legacy）与 `activity/<id>`（internal）全链路区分；同整数 id 不串线。
- **统一接口**：`createActivity/activityByRef/currentActivitiesUnified/hasActiveActivity/nextDueActivity/claimActivity/finishActivity/deferActivity/applyActivityAction/setActivityShareIntent/recoverActivities`；状态 active/paused/completed/abandoned，版本 CAS、runId、来源、单 active 跨存储事务校验。
- **前台**：`saveQATurn` 的 personal_interest 新活动写 internal_activity，不再写 `chat_history.thought`；privateWriting=false 不创建；幂等 runId。
- **OpenWake**：`decision=start_activity` 可选（不强制），单事务完成 OpenWake + 创建 internal activity；rest/reflect 仍合法；无 contact/outbox/chat_history 用户行。
- **ActivityReview**：统一读取/领取/提交 legacy 与 internal；internal 分享只保存 `share_intent`，不发送；legacy 分享链与 Outbox 保持不变。
- **感知**：前台/回顾/快照统一读取活动 ref/status/version/goal/progress/draft/artifact/next_step；completed 不再以过时 next_step 出现在 current；群聊/第三方隔离。
- **边界**：本片不实现 R12-2 contact/outbox；模型写出作品≠已分享，分别记录。

## R12-2a 已落地（暂停点）

- v32 迁移新增 `action_proposal`（action_key 唯一、origin/run_id/scope/context_epoch/target、payload/claims、state 机、expires、committed_action_key）；单事务幂等；**未接 Outbox/发送**。
- `ActionOrigin`：kind/ref/id/version/runId/scope/contextEpoch/target；`wake|reflection|concern:<id>:<version>:contact`、`thought|activity:<id>:<version>:share`；拒绝伪造/负数/串线。
- `EvidencePurpose`：HistoricalFact（不因早于 wake stale）/CurrentState/NewEvidence（唯一 stale_ref）/UserCommitment/ExecutionResult/ArtifactResult/InternalState（不得当用户事实）；activity_share 支持新旧 ActivityRef 且要求真实完成成果。
- 统一 `validateActionProposal`：origin 身份/版本/action_key/target、claims purposes、artifact 真实、coverage（claims=[] 也执行）、semantic_status 服务端；`allow_contact=false` 拒绝 OpenWake contact。
- 提案持久化 API：persist/actionProposal/update CAS/expire/mark committed；幂等与冲突明确。
- 验证：全量 `ctest 23/23`；`tst_memorymanager 146/1 skipped`、`tst_memoryagent 93`、`tst_actionpolicy 17`、`tst_cognitivesnapshot 15`、`tst_actionoutbox 15`。生产库仍 v31（未静默迁移）；迁移测试在临时库通过。
- **未做**：R12-2b 提交路径/Outbox/回执/真实 verifier 请求、R12-3、后台 tool loop。

## R12-2b 已落地

- `ActionCommitter` 新入口 `commitVerifiedActionProposal/commitOpenWakeContact/commitInternalActivityShare/drainVerifiedActionProposals`：读取 verified 提案，复验 ActionPolicy/来源/版本/runId/scope/epoch/target/成果/开关/安静/共享额度，单事务写 `cognitive_action + action_outbox + delivery_ledger` 并 CAS `verified→committed`；失败 hard→withheld，可恢复→保留 verified 等 drain。
- OpenWake contact：`allow_contact` 独立默认 false；false 时 contact 拒绝/保留内部记录、不记认知失败；true 时服务端 ActionOrigin `wake/<id>:<version>`，确定性+coverage（claims=[] 也跑）+恰好一次 semantic verifier + ≤1 次 bounded repair；通过后 persist→verified→commit；不伪造 chat/reflection。
- internal activity share：绑定真实 completed artifact_ref/version/hash 与 share_intent，action_key `activity:<id>:<artifact_version>:share`；只入 Outbox，不改变活动完成状态；legacy share 链不变。
- 回执：`actionSettled` 支持 `wake:`/`activity:`，`recordActionReceipt` 幂等回写 open_wake last_outcome / activity share_outcome/version；成功才写主动历史；unknown 不写成功/不重发；启动 `drainVerifiedActionProposals` + 回执对账。
- 验证：全量 `ctest 23/23`；`tst_memorymanager 146/1 skipped`、`tst_memoryagent 94`、`tst_actionoutbox 20`、`tst_actionpolicy 17`、`tst_cognitivesnapshot 15`。生产库 v31→v32 迁移后旧行数不变（chat 534/ledger 137/outbox 0），`allow_contact=false` 启动零新发送。**未做**：真实 QQ/pet 目标发送（需另行授权）、R12-3、后台 tool loop。

## R12-2c 已落地（2026-11-27）

- **c1 语义核验可信边界**：verifier 输入按完整证据项分配预算、不再字符串截断；核心证据放不下时 `evidence_budget_exceeded` 并 withheld；request_id/action_key/origin/runId/payload_digest 全链路关联；严格 coverage/result 枚举白名单；在途请求按 request/action 管理，迟到/被取代回调丢弃；自由重写 repair 默认 fail closed（每 action ≤1 verifier、0 repair）；semantic_status 服务端白名单。
- **c2 事务/Fence/发送前复验**：v33 增加 verified 指纹/request_id/verified_at/history_projected/cancel_reason/action_context_epoch；提交事务内重读提案、复验 state/来源/版本/runId/scope/target/expires/成果 hash/已验证内容指纹；额度事务内统计；ledger 写入失败回滚；action_key 幂等校验身份；Action Context Epoch 持久化在 attention_state，主人私聊输入/事实更正/撤销 bump，群聊不 bump；claimOutbox 排除 withheld/unknown/failed 不可重试，领取与 sender 前、QQ 入队前逐层复验，永久失效取消、可恢复保留。
- **c3 回执与历史闭环**：`recordActionReceipt` DB 权威校验 + 同事务历史投影 + `history_projected` 幂等；unknown/withheld 迟到成功不写历史；不存在/未提交/渠道不匹配拒绝；对账按未投影扫描，不再只扫最早 64；旧 wake 不覆盖新 last_outcome；活动分享回执绑定 artifact_ref/version/hash；分享意图崩溃恢复（有界游标/节流/版本撤回失效），不自动加入历史成果；调度日志 UTC/local/epoch；每次尝试 run_id 回填；迟到失败按版本隔离；120/300/900 + 第 4 次暂停语义保持。
- 验证：全量 `ctest 23/23`；`tst_memorymanager 154/1 skipped`、`tst_memoryagent 100`、`tst_actionoutbox 38`、`tst_actionpolicy 18`、`tst_cognitivesnapshot 15`。**生产库仍 v32、未启动应用、未真实发送**；v33 迁移已在临时库测试，待独立评审后迁移。

- 全量 `ctest 23/23`；`tst_memorymanager` 139 passed/1 skipped、`tst_memoryagent` 91、`tst_cognitivesnapshot` 15、`tst_actionoutbox` 15、`tst_actionpolicy` 9。
- 生产库 v31 无 schema 变更；R12-1b 无新迁移。
- 真实运行：v31 OpenWake rest/reflect 已实证；R12-1b 注入临时 internal activity 后 ActivityReview 实际领取 `activity/1` 并调用模型，但模型返回契约外 `status`，服务端正确拒绝并进入持久退避，未写 chat/outbox/ledger；临时活动/失败行已清理。internal 活动成功完成的路径由 `activityReviewInternalClaimFinishAndShareIntent` 等单测覆盖。
- 真实模型输出契约（尤其 activity status 四值）仍需后续观察。
- 旧提醒、旧活动分享、Outbox 回归全通过。

## 已确认的设计约束（R12-2 起）

- 活动存储 A：独立 `internal_activity`；不伪造 chat_history；不强制迁移历史。
- R12-2 再做 OpenWake contact、统一证据/Authorization/Outbox；不得新增绕开路径。
- 后台通用 tool loop 后置。

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


## R12-2c-final 行动与恢复

- 新来源核验按引用顺序读取完整必要证据。预算不足时不发 verifier 请求。核验回调、行动提交和协议发送前复核实际证据摘要。v33 的 `verified_claims_hash` 保存声明与证据的联合摘要，早期只有声明摘要的待发提案会关闭。测试辅助签发入口位于测试代码。
- OpenWake 保留发起回顾时的行动 epoch。设置热更新或启动时发现权限收缩会推进 epoch，恢复权限不会复活该 epoch 的旧行动。Outbox 管理的 QQ 动作在逐句等待后复核权限，并由 Outbox 管理唯一重试预算。已委托提醒按自身状态与有效期复核。
- 关注的对外动作被拒时，内部记录和修订仍完成。原关注等待新的依据，取消单纯由拒绝产生的定时领取。
- 检索线程为每次任务建立和清理独立连接。索引 SQL 出错会返回 degraded。启动对账保留已有历史和活动结果，补索引并将没有运行中处理者的旧通知落到终态。
- `tst_memoryagent` 的真实模型验收通过环境变量获得测试凭据。测试库位于临时目录，投递由本地 mock sender 接收。Q01–Q05 使用本机 WebSocket 实际协议帧。

### 隔离验收结果

| 范围 | 结果 |
| --- | --- |
| E01–E03 | 普通证据和 2890 字符请求提交成功。超预算返回 `evidence_budget_exceeded`，没有 verifier 请求。 |
| E04 | 新来源路径没有可选证据，此项不适用。 |
| E05–E08 | 回包前改正文、核验后改正文、提交后改正文均被拦截。SQL 读取顺序变化仍能提交。冲突 key 与缺失来源均无法提升为 verified。 |
| Q01–Q05 | 合法动作收到一帧。撤权和主人插话后的待发动作收到零帧。协议拒绝后撤权时保持一帧，队首取消后后续动作通过。 |
| V / C / R / D | 现有核验错误、晚到回调、运行身份、失败隔离、公平调度和恢复用例进入全量回归。已回退的旧编号用例缺少原始清单，结果以现有用例与上表为准。 |
| 真实模型 | DeepSeek OpenWake 联系和内部成果分享各核验一次、各交给 mock sender 一次并写回历史。休息场景完成且发送零次。真实 verifier 拒绝了有合法引用但缺乏文本依据的声明。 |
| 数据库 | 真实 v32 备份副本升级到 v33，v33 备份恢复副本可读。两份副本的完整性、外键和关键表行数检查一致。 |

- 分享恢复会领取尚未请求核验的意图。如果进程在核验结果落库之前中断，已登记请求的成果保留在活动记录里，分享记为 `verification_outcome_unknown`。
