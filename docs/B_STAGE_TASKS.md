# B 阶段剩余任务施工单（B1–B5）

> 更新：2026-11-23 · 基线提交 `38af0b7`（11/22 P0 版本）
> 依据：`构想.txt`、`千岛茉子_统一检索与记忆感知重构方案_2026-11-13.md`、[B 阶段方案](B_STAGE_CONTINUITY_PLAN.md)、`ROADMAP.md`
> 纪律：**不面向结果**——只实现通用机制；验收断言对应机制，不对特定词/特定日志特判。

## 0. 日志证据（`logs_20261122_235021_131_08ba87506fcb.txt`）

| 观察 | 结论 |
|---|---|
| `[Concern] provided` 出现 7 次；`prior_reason=match/ranked` 均有 | P0 的解析器与后台 prior 已生效 |
| `reflection_change` 由 0 变 3（`revise 529←480`、`resolve 528←509`、`revise 527←519`） | 后台修订链能跑 |
| `concern_resolution` 仍 0；回复中无 `concern_updates` | 前台"答案了结"正向未验证（该轮对话没有回答开放关注） |
| `reflection/514` 在照片话题被注入 | 匹配过宽：称呼类高频片段（"欧尼酱"）单独成匹配 |
| `reflection_contact` 无 `now`；`[ReflectionContact]` 0 行 | 主动联系仍不可达（无到期入口 + 提示词抑制） |
| `[Cognition] ...` 无新死信；`cognitive_source 179` 转 `updated` | 队列可消退 |

## 1. 状态：ROADMAP B1–B5 未勾选项

| 项 | 缺什么（当前基线） |
|---|---|
| **B1 统一事件与注意力** | 只有"新消息 / 60s 空闲回顾"两条入口；到期唤醒、活动结果、恢复事件没有统一事件队列；没有"当前活动 + 切换原因"；没有延后消息/暂停活动的显式状态 |
| **B2 持久化念头与活动** | 活动进度寄存在 `chat_history.thought_result`；没有来源/目标/真实产物字段；取消与来源删除有部分防护；多活动、迟到提交防护需补 |
| **B3 自主回顾与新目标** | 回顾只由新来源触发；没有"到期关注"再进入；没有"无待处理事项直接退出"的可观测理由；预算可配置但"新目标形成"只有 `personal_interest` 一次性入口 |
| **B4 主动交流与执行反馈** | `contact now` 从未被选择/从未投递；投递无 attempts/回执汇总；主动消息不写历史，下次无法回认；草稿分享无回执 |
| **B5 连续运行验收** | 无跨场景 fixture、无观测清单、无重复发送/取消/放弃的回归装置 |

## 2. 里程碑与文件所有权

> 每个里程碑独立可构建、可回滚；源码单写者（lead 或指定 agent），构建只有 lead 执行。

### M1 · B3 关注生命周期 + 到期回顾（进行中）

| 子项 | 文件 | 验收 |
|---|---|---|
| v23 迁移：`private_reflection` 加 `topic/watch/root_turn/next_check_at/resolved_by/resolved_kind/matter/claimed_by` | `memorymanager.cpp` | 新库/旧库重开 `user_version=23`；旧 36 条关注回填 `root_turn`、`next_check_at` |
| 匹配精度：文档频率降权（多关注语料内高频片段不计数） | `privatereflection.cpp` | 称呼类片段不再单独命中；同话题仍命中；`beforeReported` 生效 |
| `claimDueConcern/concernReviewContext/finishConcernReview/deferConcernReview` | `privatereflection.cpp` | 到期关注无需新消息即可领取；keep/revise/resolve/drop/contact 全部可提交；重复领取为空；预算计入 `thought_review_log` |
| 协调器优先级：到期活动 → 到期关注 → 新来源 | `privatereview.*`、`cognitivecoordinator.*` | 三者互斥、单在途；`private_writing=false` 时到期关注仍可被领取 |
| 提示词：`concern_action` 契约（keep/revise/resolve/drop/contact） | `llmservice.cpp` | 请求带 `kind=concern` 上下文；要求 `now` 必须有理由；内部打分字段不进入模型 |
| 测试 | `tests/tst_concern.cpp`、`tst_memorymanager.cpp` | 迁移、到期领取、三种终态、DF 精度、版本断言 |

### M2 · B4 主动联系可达 + 执行反馈 + 回认

| 子项 | 文件 | 验收 |
|---|---|---|
| 主动联系门控：`canContactNow` 与 `privateWriting` 解耦（只看 `proactive.enabled`、空闲、静默时段、目标能力） | `appcontroller.cpp`、`cognitivecoordinator.*` | 只开主动联系、关写作时仍能投递；关主动联系时 `withheld` |
| 决策理由：`now` 必须带 reason + 触发来源（新证据/到期/活动结果）；提示词给出判定顺序 | `llmservice.cpp` | 无理由 `now` 提交被拒（服务端校验），有理由可投递 |
| 投递状态：`reflection_contact` 加 `attempts/last_outcome/last_attempt_at`；发送失败可重试一次且不重复；`unknown` 不重发 | `memorymanager.cpp`、`privatereflection.cpp`、`appcontroller.cpp` | DB 状态机完整；重启后 `dispatching` 有明确归宿；无重复消息 |
| 主动消息写历史：`chat_history(turn_kind='proactive', user_input=NULL/空)` + `raw_events(direction='out')`；`promptassembler` 已有主动轮渲染 | `appcontroller.cpp`、`memorymanager.*` | 下次对话历史里能看到"茉子主动说过的话"，并可作为 `prior_assistant_expression` 引用 |
| 分享回执：`deliveryEcho="share:thought/<id>:<version>"`；结果写 `thought_result.share_outcome`；已送达不重复分享 | `appcontroller.cpp`、`memorymanager.cpp` | 草稿分享有结果；重复分享被 guard 拒绝 |

### M3 · B1 统一注意力事件 + B2 活动持久化补全

| 子项 | 文件 | 验收 |
|---|---|---|
| `attention_event` 表：`kind ∈ message/due_wake/activity_result/recovery/contact_result`，状态 `pending/claimed/done/abandoned`，payload JSON | `memorymanager.cpp` | 事件幂等入队；协调器按优先级领取；重启后未完成事件可恢复 |
| 当前活动与切换原因：`attention_state`（当前活动 ref、切换原因、since、版本） | `memorymanager.*`、`cognitivecoordinator.*` | 新消息打断活动时记录 `paused/switch_reason`；重启恢复当前活动 |
| 延后/暂停/改期：活动 `activity_action` 已有 pause/resume/abandon/reschedule；补"延后处理消息"的显式记录 | `cognitivecoordinator.*`、`memorymanager.*` | 消息可延后而不用丢弃；延后原因可审计 |
| B2 活动持久化：`thought_result` 增 `source_turn/goal_kind/artifact_ref/created_at`；取消与来源删除阻止迟到提交/分享；多活动选择按版本 | `memorymanager.cpp`、`llmservice.cpp` | 重启后读回来源/目标/产物；取消后迟到回调不复活；删除来源后不可分享 |
| 元数据优先的"是否阅读"（B1 条件项） | 暂缓并记录 | 仅在群聊非@消息上试点，不阻塞其余验收 |

### M4 · B5 连续运行验收装置

| 交付 | 内容 |
|---|---|
| fixtures | `tests/fixtures/b_stage/`：关注生命周期、到期联系、重启恢复、打断、取消/放弃、分享回执 6 个场景 |
| 测试 | `tst_concern`（记忆层）、`tst_memoryagent`（协调器/投递）、新增 `tst_attention`（事件队列与恢复） |
| 观测清单 | 每次唤醒输出：触发来源、领取 id/version、决策、持久状态、投递结果、切换原因 |
| 实机验收脚本 | 7 天观察表：到期关注处理率、`now` 理由可核对率、重复追问率、投递回执率、重启恢复 |

## 3. 验收矩阵（B5）

| 场景 | 资料 | 决定 | 持久状态 | 真实产物 | 投递结果 |
|---|---|---|---|---|---|
| 多次唤醒 | 每次上下文含 since/prior | keep/revise/contact 三态 | `state/version/next_check_at` 递增正确 | 无或草稿 | 有回执/无重复 |
| 用户打断 | 打断时保存 `deferred_reason` | 恢复后重新决定 | 状态不被迟到回调覆盖 | 草稿保留 | 不重复发 |
| 重启 | 从库读回关注/活动/事件 | 不重复决定 | `claimed_by/eligible_at` 正确 | 草稿/产物读回 | `unknown` 不重发 |
| 失败 | 网络/截断/渠道失败 | 不虚构结果 | 失败状态落库 | 无 | `rejected/unknown` 可查 |
| 取消/放弃 | 来源删除/用户取消 | 不再行动 | 终态 | 不可分享 | 不投递 |

## 4. 明确不做（本阶段）

- 不做完整"先看不看消息再切换活动"的注意力引擎（重构方案 §1.2 明确不扩大）；B1 的元数据优先项只留接口与记录。
- 不引入新框架/向量库；检索沿用现有 `MemoryRetrievalService`。
- 不为通过验收写特判；所有阈值（DF、时限、预算）用通用规则并在文档记录。

## 5. 完成状态（2026-11-23）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| M1 · B3 关注生命周期 + 到期回顾 | **完成** | v23 迁移；`claimDueConcern/concernReviewContext/finishConcernReview/deferConcernReview`；`PrivateReview` 三级优先级；DF 匹配；`tst_memorymanager`/`tst_concern` 覆盖 |
| M2 · B4 主动联系 + 反馈 + 回认 | **完成** | v24 迁移（`reflection_contact` 尝试字段、`delivery_ledger`）；`canContactNow` 解耦；`rejected` 补一次/`unknown` 不重发；主动消息写 `chat_history(proactive)` + `raw_events(out)`；分享回执与去重 |
| M3 · B1 事件 + B2 持久化 | **完成（工程路径）** | v25 迁移（`attention_event`/`attention_state`）；协调器恢复扫描、due_wake、消息事件与当前活动/切换原因；活动 `source_turn/goal_kind/artifact_ref/updated_at`；无产物不可分享 |
| M4 · B5 验收装置 | **交付，待实机** | `docs/B_STAGE_ACCEPTANCE.md`（6 场景 + 7 天观察表）；自动测试已覆盖状态机与恢复 |

验证：全量构建零错误；`ctest` 19/20（唯一失败 `tst_runtimelog` 属宿主进程管道环境问题）；`tst_memorymanager` 77 passed；`tst_concern` 通过；`tst_memoryagent` 通过。生产库已备份并完成 v22→v25 迁移（`user_version=25`，36 条开放关注回填）。

**仍未完成（不冒充已验收）**：B5 的 7 天实机连续运行、真实 QQ 主动联系到达、多活动并行选择、元数据优先阅读门控。

## 6. 改进批次（M5，2026-11-23）

针对"重要事件记忆、能力自感知、主动选项、设置项、日志命名"五个问题的落地：

| 问题 | 实现 | 验收 |
|---|---|---|
| 重要事件更强记忆 | `MemoryEvidence.importance` 从 `episodic_memory` 进入证据；启发式重排 `+0.75*importance`（无 importance 时分数不变）；衰减仍保护 `importance>=0.8` | `tst_memorymanager::episodicImportanceBoostsRerank`：同词面下高 importance 排前 |
| 能力自感知 | `LLMService::capabilityManifest(meta)` 按配置生成 memory_recall/reminder/private_contact/private_review/vision/tts；前台注入只读清单，回顾提示词注入一行授权摘要 | `tst_memoryagent::capabilityManifestReflectsConfig`、`foregroundPromptCarriesCapabilityManifest` |
| 主动选项额度 | 新配置 `proactive.contact_daily_limit`（默认 3，0 不限）；`claimReflectionContact` 按 `delivery_ledger` 当日计数拦截 | `tst_memorymanager::contactDailyLimitBlocks` |
| 设置项 | 设置页新增联系日限额、关注默认检查间隔、关注保留期三项；config 解析与夹紧 | `tst_configmanager`、`tst_settingspages` |
| 日志命名 | 文件名保持 `logs_<ms时间戳>_<随机会话id>`；新增会话头 `app/version/pid/qt/build/exe_dir/config/config_hash(SHA-256 前12)` | `tst_runtimelog::sessionHeaderDescribesRunAndConfig` |

同时落地：`proactive.concern_default_check_minutes`（默认 720）替换硬编码默认；`proactive.concern_retention_days`（默认 14）限制到期回顾范围（新证据匹配不受限）。

验证：全量构建零错误；`ctest` **20/20**；`tst_memorymanager` 81 passed / 1 skipped、`tst_memoryagent` 53 passed、`tst_configmanager` 16、`tst_settingspages` 3。

**M5 仍未做**：能力清单的"当前活动/正在做的事"注入（只持久化未注入）；日志保留期/轮转；同义改写匹配；误了结 reopen；多活动并行。
