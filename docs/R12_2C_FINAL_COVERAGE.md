# R12-2c-final 验收项与现有自动测试覆盖映射

- 生成时间：2026-11-27 23:20–23:35（+08:00），按只读方式生成：未改动 `src/`、`tests/`、`build/` 下任何既有文件，未启动应用、未发送任何真实 QQ/Pet 消息、未发起真实模型请求；唯一新增文件为本文件。
- 代码基线：`HEAD=4dcafb6ffbcf438fde489399bd19315814dbb267`，`HEAD^{tree}=c239b5e75228c5760d9321a96a603648216c7fe8`。仓库内 `a9289c2`（报告原文所记 SHA）的 tree 与 HEAD tree 完全相同（同轮基线核对见 `docs/R12_2C_FINAL_BASELINE.md`），因此下文按 HEAD 树映射。
- 证据基线：`Windows_AI.exe`（`build/stage-b/Windows_AI.exe`，2026-11-27 22:38:06）与 `tst_memoryagent.exe`（`build/stage-b/tests/memory-agent/tst_memoryagent.exe`，22:43:14）均构建自 HEAD 树源码（src mtime 22:37:27；HEAD 仅改动 `tests/tst_memoryagent.cpp`，该目标已单独重建并复跑）。
- **工作区现场说明**：本文件写成的过程中（23:10 之后）工作区出现并行 R12-3a 工作流的未提交改动（`src/cognition/cognitivecontext.h/.cpp`、`src/cognition/llmservice.h/.cpp`、根 `CMakeLists.txt`、`tests/CMakeLists.txt`，并新增待构建目标 `tst_cognitivecontext`）。这些改动不属于 R12-2c-final 基线证据，也未被本文件的任何结论引用。**判定"树一致"时应以 `git rev-parse HEAD == 4dcafb6...` 且除本文件外工作区干净为准**；若直接从当前含 R12-3a 改动的工作区重建，构建产物会混入 R12-3a 代码，本文 23/23 等结论不能直接沿用。
- 主要数据来源：`docs/R12_OPEN_WAKE.md:105-125`（R12-2c-final 行动与恢复 + 隔离验收结果表）；`tests/CMakeLists.txt`（23 个测试目标）；`tests/tst_*.cpp` 与 `tests/testactionverification.h`；`build/stage-b/` 下 ctest 与矩阵日志（Qt 测试日志为 **GBK** 编码，直接按 UTF-8 读取会乱码）；`build/stage-b/r12-*/` 内真实模型/迁移证据目录。

---

## 0. 结论摘要（TL;DR）

1. **E01–E03（证据/预算）**：有明确构造的用例，且最终矩阵日志给出逐场景实际字节数（normal=1140、near-budget=2890、over-budget=4112 → `evidence_budget_exceeded` 且零 verifier 请求）。
2. **E04**：原表判定"新来源路径没有可选证据，此项不适用"，无相应用例，标记 `not_applicable`。
3. **E05–E08（内容篡改/绑定拦截）**：回包前改正文、核验后改正文、提交后改正文三条篡改路径，加上 SQL 读取顺序变化、缺失来源、同 key 冲突内容，均有显式构造与断言。
4. **Q01–Q05（QQ 协议帧）**：`tst_actionoutbox::qqOutboxProtocolBoundary` 在本机 `QWebSocketServer` 上收发**真实 JSON 协议帧**，四行数据场景覆盖 Q01–Q05（含撤权/插话后零帧、协议拒绝后撤权保持一帧、被拦动作之后 tail 帧仍通过）。
5. **V/C/R/D**：**原始编号清单不可恢复**（`docs/` 从未提交过 R12-2C 报告原文，git 历史与全仓检索均无；`docs/R12_OPEN_WAKE.md` 只有汇总表）。本文按语义分 6 组映射；源码注释中残留的 `V01–V14` 标签只作为辅助定位，不等于原始清单。
6. **真实模型**：最终矩阵日志 `r12-acceptance-final/r12-matrix.log` 显示 15/15 通过，其中 verifier 为真实 `deepseek-v4-flash`、`http=200`（latency 约 1.1–2.6s、返回 token 计费），可证明真实模型验收已跑过；但真实模型非确定，**重跑不保证稳定**（A-contact 曾在 22:38/22:41 两次失败，见 §8 与 §10-U9）。
7. **数据库**：真实 v32 副本升级到 v33、v33 副本可读有应用日志与 SQLite 头部（`user_version=33`）佐证；但"完整性/外键/关键表行数检查一致"的原始输出**未入库，不可复核**。
8. **覆盖计数**（63 个映射条目，见 §1）：`explicit=59`、`generic=2`、`unrecovered=1`、`not_applicable=1`。另有真实模型 4 项、数据库 4 项单列（§6），不计入 63。
9. **23 个测试目标**：HEAD 下确为 23 个（`tests/CMakeLists.txt`）；最近一次全量 ctest 为 `build/stage-b/r12-at-most-once-ctest.log`（22:40:07）**100% 23/23 通过**；此后 HEAD 只改测试文件，`tst_memoryagent` 重建后单目标 ctest **1/1 通过**（22:43:56，24.57s）。

---

## 1. 覆盖强度口径与计数

| 标记 | 定义 |
| --- | --- |
| `explicit` | 存在测试函数/数据行**明确构造**该验收场景，并断言关键行为、状态或诊断码 |
| `generic` | 未专门构造该场景，仅由更泛化用例间接覆盖关键路径，或代码路径存在但边界未被断言 |
| `unrecovered` | 无对应用例（含应用层接线未纳入测试构建） |
| `not_applicable` | 原验收项本身不适用 |

计数（与 §2–§5 表格逐行对应；"条目"=E 系列 8 + Q 系列 5 + V/C/R/D 语义 44 + 行动与恢复补充 6 = 63）：

| 覆盖强度 | 条目数 | 明细 |
| --- | --- | --- |
| explicit | **59** | E 7（E01–E03、E05–E08）+ Q 5 + V/C/R/D 44 + 行动恢复补充 3 |
| generic | **2** | S2「v32 遗留旧 verified 行关闭路径」、S5「已委托提醒有效期复核」 |
| unrecovered | **1** | S3「权限收缩推进 action epoch / 恢复权限不复活旧行动」的 AppController 接线 |
| not_applicable | **1** | E04 |
| 合计 | **63** | |

另有真实模型 4 项、数据库 4 项见 §6（不并入上表，避免与 E/Q 条数混算）。

---

## 2. E 系列：证据绑定与证据预算

对应 `docs/R12_OPEN_WAKE.md:117-119`。全部实现在 `tests/tst_memoryagent.cpp`。
> 编号对应说明：E01–E03 与文档三项描述一一对应，并由矩阵日志实测定界（normal `bytes=1140`、near-budget `bytes=2890`、over-budget `bytes=4112`）；**E05–E08 四号与具体用例的一一对应关系属语义推断**（原始清单未入库），此处按"回包前/核验后/提交后"三处篡改 + 其余绑定校验归并说明。

| 原验收项 | 现有测试（文件 + 函数/数据行 + 行号） | 覆盖强度 | 说明 |
| --- | --- | --- | --- |
| E01 普通证据提交成功 | `tests/tst_memoryagent.cpp` `actionEvidenceBinding(normal)`：`actionEvidenceBinding_data` L3367，row L3371；断言 L3447-3451、L3473 | explicit | 真实 OpenWake 来源 + 1 条证据，verifier 输入 `sources[0].text == evidence`，`markActionProposalVerified` 成功；矩阵日志 `bytes=1140` |
| E02 2890 字符请求提交成功 | `tst_memoryagent.cpp` `actionEvidenceBinding(near-budget)`：row L3372（`length=1780`，evidence 前缀 + 填充）；断言同上 | explicit | 矩阵日志实测 verifier 请求 `input_chars=2890`、`http=200`、通过；与文档"2890 字符"完全吻合 |
| E03 超预算不请求 verifier | `tst_memoryagent.cpp` `actionEvidenceBinding(over-budget)`：row L3374（`length=3000`）；断言 L3440-3445 | explicit | 断言 `withhold_reason == evidence_budget_exceeded` 且 `server.bodies.isEmpty()`（零 verifier 请求）；矩阵日志 `action budget_exceeded ... bytes=4112` |
| E04（不适用） | 无 | not_applicable | 原表正文："新来源路径没有可选证据，此项不适用"（`docs/R12_OPEN_WAKE.md:118`）；未构造对应用例是正确行为 |
| E05 回包前改正文被拦截 | `tst_memoryagent.cpp` `actionEvidenceBinding(changed-during-verification)`：row L3375；改库 L3454-3455；断言 L3465-3471 | explicit | verifier 返回后证据被改 → `markActionProposalVerified` 返回 false，错误 `verification_evidence_mismatch`，提案保持 `proposed` |
| E06 核验后改正文被拦截 | `tst_memoryagent.cpp` `actionEvidenceBinding(changed-before-commit)`：row L3376；改库 L3478；断言 L3479-3482 | explicit | 已 verified 后改证据 → `commitVerifiedActionProposal` 返回 `verified_content_mismatch`，不建动作/Outbox |
| E07 提交后改正文被拦截 | `tst_memoryagent.cpp` `actionEvidenceBinding(changed-after-commit)`：row L3377；改库 L3487；断言 L3488-3491 | explicit | 提交成功后改正文 → `validateOutboxActionBeforeSend` 返回 `ok=false`、`reason=verified_content_mismatch`（发送前最后一道复验） |
| E08 SQL 顺序无关 / 缺失来源 / 冲突 key 不得提升 | `tst_memoryagent.cpp`：`actionEvidenceBinding(different-sql-scan-order)` row L3373（改序 + `PRAGMA reverse_unordered_selects=ON` L3452-3453，仍能提交）；`actionEvidenceBinding(missing-during-verification)` row L3378、L3456-3457 + L3465-3469（`verification_evidence_unavailable`，保持 `proposed`）；`openWakePersistConflictDoesNotPromoteOtherContent()` L3763（断言 L3846-3860） | explicit | 三条补充校验均明确构造：证据按 ref 而非 SQL 遍历顺序取用；来源缺失无法 verified；同 action_key 已存在冲突内容 A 时，核验为 B 的结果不得把 A 提升为 verified，wake 按 reflect 收口、Outbox/动作行均为 0 |

---

## 3. Q 系列：QQ 协议帧与待发动作

对应 `docs/R12_OPEN_WAKE.md:120`。全部实现在 `tests/tst_actionoutbox.cpp::qqOutboxProtocolBoundary()`（L490；数据驱动行 `qqOutboxProtocolBoundary_data` L481-488）。用例在本机起 `QWebSocketServer`（L554-570）并走真实 `QQProtocolClient`/`QQChannelAdapter`，收发真实 `send_private_msg` JSON 帧，**不是**对 sender 的纯 mock。

| 原验收项 | 现有测试（文件 + 数据行 + 行号） | 覆盖强度 | 说明 |
| --- | --- | --- | --- |
| Q01 合法动作收到一帧 | `tst_actionoutbox.cpp` `qqOutboxProtocolBoundary(Q01-accepted)`：row L484；断言 L597-601 | explicit | `frames.count(key)==1`、`actionSettled=protocol_accepted` |
| Q02 撤权后待发动作零帧 | `qqOutboxProtocolBoundary(Q02-Q05-revoked)`：row L485；撤权 L608-616；队首/尾帧 L591-595、L617-620 | explicit | 先发 `lead` 帧占用节奏等待，pending 动作发送前复验时 `allow_contact=false` → `frames.count(key)==0` |
| Q03 主人插话后零帧 | `qqOutboxProtocolBoundary(Q03-context)`：row L486；插话 L603-607；断言 L617-620 | explicit | 等待期间 `recordInput` 推进 Action Context Fence → 待发动作零帧 |
| Q04 协议拒绝后撤权保持一帧 | `qqOutboxProtocolBoundary(Q04-retry-revoked)`：row L487；协议拒绝 L565-568；断言 L621-624 | explicit | 协议端返回 `status=failed/retcode=100`，撤权后该动作只有已发出的一帧（`frames.count(key)==1`），不重发 |
| Q05 队首取消后后续动作通过 | `qqOutboxProtocolBoundary(Q02-Q05-revoked)`：row L485（行名显式写 `Q02-Q05`）；断言 L617-619 | explicit | 被拦的待发动作之后，`tail` 帧仍能发出（`frames.contains("tail")` 且 `frames.count(key)==0`），证明取消不影响后续队列；Q05 与 Q02 共用一个数据行，编号依据来自用例行名 |

协议层的逐句节奏/单次重试预算由 `tests/tst_qqdelivery.cpp::textPacingAndSlowAcknowledgement()`（L12）与 `orderedSentences()`（L52）补强：一次失败只重试一次、重复回执不推进、断连逐条回执（`queuedSendsReportedOnDisconnect` L117）。

---

## 4. V / C / R / D 语义分组映射

> **原编号不可恢复**：`docs/` 从未提交过任何 R12-2C 报告文件（全仓检索无该文件），git 历史亦无；`docs/R12_OPEN_WAKE.md:121` 只保留一句汇总："现有核验错误、晚到回调、运行身份、失败隔离、公平调度和恢复用例进入全量回归。已回退的旧编号用例缺少原始清单，结果以现有用例与上表为准。" 同轮独立基线核对（`docs/R12_2C_FINAL_BASELINE.md`）亦结论"报告原文与编号验收矩阵不可恢复"。
> 因此**本表不声称还原 V/C/R/D 原编号**，只按文档列出的语义分组做映射；`V01–V14` 仅当作测试源码注释里残留的辅助标签（`tests/tst_memoryagent.cpp:3496/3569/3650/3714/3761/3863/3135`、`tests/tst_memorymanager.cpp:6208/6302`、`tests/tst_actionpolicy.cpp:1218`），C/R/D 三个字母对应的原编号与含义完全不可恢复。

下表中 `explicit` 行的测试均来自 23/23 全量回归与 `r12-recovery-tests/storage-suite.log`（tst_memorymanager 全量 157 passed/1 skipped，22:36:27）的实际 PASS 记录。

### 4.1 核验错误（verifier 预算 / 失败闭合 / 诊断）

| 语义验收点 | 现有测试（文件 + 函数 + 行号） | 强度 | 说明 |
| --- | --- | --- | --- |
| verifier 输出预算 1024、不发 `thinking:{disabled}`、每动作恰 1 次请求 | `tests/tst_memoryagent.cpp` `verifierOutputBudgetIs1024WithoutThinkingParam()` L3499；`openWakeContactVerifiedAndCommitted()` L3052（请求计数） | explicit | 源码注释 V01；矩阵日志 verifier 请求 `max_tokens=1024` |
| 核验完成日志记录 finish_reason/usage 且不含正文 | `tst_memoryagent.cpp` `verifierCompletionLogsUsageWithoutContent()` L3571 | explicit | 源码注释 V02 |
| 响应 truncation（finish_reason=length）fail closed | `tst_memoryagent.cpp` `verifierTruncatedResponseFailsClosed()` L3652 | explicit | 源码注释 V03；withheld + 明确诊断 |
| 网络失败 fail closed | `tst_memoryagent.cpp` `verifierNetworkErrorFailsClosed()` L3715 | explicit | 源码注释 V04；`network_error` 诊断 |
| 响应缺 request_id / 未知枚举 / request_id 不匹配 → withheld | `tst_memoryagent.cpp` `verdictProtocolViolationsAreWithheld()` L2271 | explicit | 协议白名单 + 关联字段校验 |
| repair 默认 fail closed（非核心 claim 无法证明时不二次改写） | `tst_memoryagent.cpp` `repairIsFailClosedWithoutSecondGeneration()` L2199 | explicit | 失败闭合，不进行自由重写 |
| verifier 输入预算按"完整证据项"分配、超预算显式诊断不截断 JSON | `tst_memoryagent.cpp` `verifierInputBudgetKeepsWholeEvidenceItems()` L2019、`budgetExceededWithholdsWithoutVerifier()` L2119 | explicit | 源码注释 A04；断言不出现 `left(3000)` 残缺 JSON |
| 严格 verdict 白名单（封闭枚举/类型） | `tests/tst_actionpolicy.cpp` `verifierVerdictStrictWhitelist()` L1318；`verifierUnavailableConservative()` L421 | explicit | 解析层 fail closed |

### 4.2 晚到 / 被取代回调

| 语义验收点 | 现有测试（文件 + 函数 + 行号） | 强度 | 说明 |
| --- | --- | --- | --- |
| 旧 generation 的在途 verifier 晚到不得污染新一轮回顾 | `tst_memoryagent.cpp` `staleVerifierCannotPolluteNewReview()` L2349 | explicit | 源码注释 A05；晚到结果被丢弃 |
| epoch 推进后迟到 verifier 结果不得创建/提升提案 | `tst_memoryagent.cpp` `lateVerifierResultAfterEpochBumpCannotPromote()` L3864 | explicit | 源码注释 V11；核验期间 Action Context Fence 推进 |
| 每次回顾尝试独立 run_id；旧版本迟到失败不污染新版本失败状态 | `tests/tst_memorymanager.cpp` `reviewAttemptRunIdAndLateFailureIsolation()` L9031 | explicit | 源码注释 A26 |

### 4.3 运行身份（runId / origin / version / epoch）

| 语义验收点 | 现有测试（文件 + 函数 + 行号） | 强度 | 说明 |
| --- | --- | --- | --- |
| verified 必须携带服务端 `VerificationBinding`，且与本次核验输入/来源/target/fence 一致 | `tst_memoryagent.cpp` `openWakeContactVerifiedAndCommitted()` L3052，断言 L3135-3168 | explicit | 源码注释 V05；binding 逐字段比对 |
| binding 与权威提案逐项不一致即 fail closed（不在事务内重算"当前 DB 内容"当已核验） | `tst_memorymanager.cpp` `verificationBindingBindsVerifierContent()` L6211 | explicit | 源码注释 V06 |
| verified 之后提案内容被改写，提交必须拒绝 | `tst_memorymanager.cpp` `commitRejectsContentChangedAfterVerifiedBinding()` L6304 | explicit | 源码注释 V09；落 withheld，不建动作/Outbox |
| verified 后 payload 被改写 → 提交按内容指纹拒绝 | `tests/tst_actionoutbox.cpp` `verifiedContentTamperIsRejected()` L1651 | explicit | 源码注释 A07 |
| 同 key 已存在冲突内容 A 时，核验结果 B 不得把 A 提升为 verified | `tst_memoryagent.cpp` `openWakePersistConflictDoesNotPromoteOtherContent()` L3763（断言 L3846-3860） | explicit | 源码注释 V08 |
| 并行分享/联系按 request_id 关联，不互相覆盖/串线 | `tst_memoryagent.cpp` `parallelShareAndContactVerifierIdsDoNotCross()` L2429（V10 注释 L2528） | explicit | 源码注释 A06/V10 |
| origin kind/ref/id/version/runId/epoch 严格校验，缺失/伪造一律 `stale_callback` 丢弃 | `tst_memoryagent.cpp` `originMismatchRejected()` L4331、`strictOriginRequiredForAllSources()` L4442 | explicit | 四类来源逐一构造缺失/伪造 origin |
| 服务端覆盖模型自填 `_origin` | `tst_memoryagent.cpp` `llmServiceOverridesModelOrigin()` L4614 | explicit | 模型伪造 origin 不生效 |
| ActionOrigin 身份隔离（来源键/版本/runId 不串） | `tests/tst_actionpolicy.cpp` `actionOriginIdentityIsolated()` L504 | explicit | — |
| claim type → EvidencePurpose 边界（V12/V13/V14） | `tests/tst_actionpolicy.cpp` `openWakeEvidencePurposeHistoricalVsNewEvidence()` L1223（V12 L1263、V13 L1282、V14 L1303） | explicit | 历史事实/新证据/self_desire 三种边界 |
| `action_context_epoch` 持久化、单调递增、仅相关语境 bump、重启可读 | `tst_memorymanager.cpp` `actionContextEpochPersistsAndBumps()` L5970、`actionContextEpochScopedAndLongIntervalPreserved()` L9100 | explicit | 源码注释 A27 |

### 4.4 失败隔离 / 退避 / suspended

| 语义验收点 | 现有测试（文件 + 函数 + 行号） | 强度 | 说明 |
| --- | --- | --- | --- |
| 运行时使用 MemoryManager 持久失败状态机（Reflection/OpenWake），用户 yield 不增计数 | `tst_memoryagent.cpp` `persistentFailureWired()` L4661 | explicit | 源码注释"失败接线"；不匹配 origin 按 `stale_callback` 丢弃 |
| 连续失败按 120/300/900 退避、第 4 次 `suspended(86400)`、到期恢复并允许超出 24h 窗口重领 | `tst_memorymanager.cpp` `reviewFailureBackoffAcrossSources()` L6367、`reviewFailureSuspendedRecoversBeyondWindow()` L6680 | explicit | 日志实测 120/300/900 |
| 内容实质修订重置计数；正常领取的 version 增长不重置 | `tst_memorymanager.cpp` `reviewFailureContentRevisionResetsCount()` L6555 | explicit | — |
| 策略/权限拦截与用户插话 `countsAsFailure=false` | `tst_memorymanager.cpp` `reviewFailureNonCountingDoesNotBackOff()` L6599 | explicit | — |
| 源删除清理失败行并审计；写失败回滚；管理员重置 | `tst_memorymanager.cpp` `reviewFailureDeletedSourceIsAudited()` L6738、`reviewFailureWriteFailureRollsBack()` L6767、`reviewFailureResetAndClear()` L6801 | explicit | — |
| Activity 失败退避与 stage/diagnostic 审计 | `tst_memorymanager.cpp` `failureBackoffSupportsActivityRefs()` L7834、`activityReviewFailureDiagnostics()` L8850 | explicit | — |

### 4.5 公平调度 / 每日预算

| 语义验收点 | 现有测试（文件 + 函数 + 行号） | 强度 | 说明 |
| --- | --- | --- | --- |
| OpenWake 到期后先让出 3 次普通回顾，第 4 次领取 OpenWake 并清零公平计数 | `tst_memoryagent.cpp` `openWakeFairnessAfterNonOpenJobs()` L4279 | explicit | 断言 `non_open_since_due=3` 后 OpenWake 领取 |
| enabled=false / daily_limit=0 / 低预算取整 / 共享比例不越界 | `tst_memoryagent.cpp` `openWakeDisabledAndBudget()` L2903；`tst_memorymanager.cpp` `openWakeScheduleAndBudgetRounding()` L6842、`openWakeBudgetGatesAndCooldown()` L6917 | explicit | 均断言零模型调用/不越界领取 |
| 公平计数按实际成功领取持久化、重启不清零 | `tst_memorymanager.cpp` `openWakeFairnessCounterPersists()` L7148、`openWakeFairnessPersistsAcrossFailureAndRestart()` L8997（A25） | explicit | — |
| `max_per_day` 只约束 OpenWake 领取（不是 contact/提醒次数） | `tst_memorymanager.cpp` `schedulingBudgetSemantics()` L8925 | explicit | 源码注释 A24 |
| OpenWake 间隔在提交边界夹紧 min/max，非法协议才失败 | `tst_memoryagent.cpp` `openWakeIntervalClampedAtCommit()` L4120 | explicit | — |
| 两个独立连接共享 contact 每日额度不超额 | `tst_actionoutbox.cpp` `separateConnectionsHonorSharedContactQuota()` L1805 | explicit | 源码注释 A11 |

### 4.6 重启恢复 / 对账 / 租约

| 语义验收点 | 现有测试（文件 + 函数 + 行号） | 强度 | 说明 |
| --- | --- | --- | --- |
| 启动时把崩溃遗留 received 轮次登记 recovery，`reviewIfDue` 先恢复再回顾 | `tst_memoryagent.cpp` `reviewIfDueRecoversInterruptedTurn()` L1507 | explicit | — |
| 租约过期仍 pending 的回顾放回可重领；OpenWake running 唯一/租约/只恢复一次 | `tst_memorymanager.cpp` `staleReflectionsRecovered()` L5121、`openWakeRunningLeaseAndRecovery()` L6958 | explicit | — |
| Outbox 单租约领取、唯一消费者 | `tst_actionoutbox.cpp` `claimOutboxSingleLease()` L735、`dispatcherIsTheOnlyOutboxConsumer()` L952 | explicit | — |
| at-most-once：dispatching 租约过期 → unknown 不重发；unknown 后迟到成功不写历史 | `tst_actionoutbox.cpp` `staleDispatchingBecomesUnknown()` L834、`unknownNeverResent()` L805、`lateSuccessAfterUnknownNeverWritesHistory()` L2145 | explicit | — |
| QQ 入队成功≠送达；入队后回执前崩溃 → unknown | `tst_actionoutbox.cpp` `qqEnqueueAwaitsReceipt()` L1099、`qqCrashBeforeReceiptBecomesUnknown()` L1174 | explicit | — |
| 回执历史投影幂等；崩溃对账补齐且唯一；多批扫描不饿死；旧 wake 回执不覆盖最新状态 | `tst_actionoutbox.cpp` `receiptDuplicateProjectsHistoryOnce()` L2105、`receiptHistoryCrashIsReconciledOnce()` L2229、`reconcileScansBeyondFirstBatch()` L2289、`oldWakeReceiptDoesNotOverwriteLatestState()` L2351 | explicit | — |
| 过期提案停机期间到期，重启只落 expired 不补发；withheld 失败行不重领 | `tst_actionoutbox.cpp` `expiredVerifiedProposalIsNotResentAfterRestart()` L1977、`withheldFailedRowIsNeverReclaimed()` L1939 | explicit | — |
| 分享恢复（崩溃窗口领取未核验意图；`verification_outcome_unknown`；失效成果不发送） | `tst_actionoutbox.cpp` `shareRecoveryAndFullLoop()` L2412（断言 L2533-2536）、`staleShareIntentIsWithheld()` L2543 | explicit | 对应 `docs/R12_OPEN_WAKE.md:125` |
| 启动对账保留历史/活动、补索引、无处理者的旧通知落终态 | `tst_memorymanager.cpp` `attentionEventLifecycle()` L5225（含 `reconcileAttentionNotifications()` 断言 L5292-5293）、`attentionRecoveryEvents()` L5302（`enqueueRecoveryEvents` 幂等 L5319-5332）、`searchIndexRebuildsAndRetrieves()` L2127、`indexWorkerRebuilds()` L2255；`tst_actionoutbox.cpp` `receiptHistoryCrashIsReconciledOnce()` L2229 | explicit | 分项显式覆盖（旧通知终态/补索引/回执对账各自有断言），未由单个端到端函数覆盖"启动对账"整体 |
| 检索线程每次任务独立连接；索引 SQL 出错返回 degraded | `tst_memoryagent.cpp` `retrievalWorkerUsesOwnConnection()` L5585；`tst_memorymanager.cpp` `retrievalDistinguishesFailedIndexFromEmptyIndex()` L766（断言 L783-784 `degraded`/`index_read_failed`） | explicit | 对应 `docs/R12_OPEN_WAKE.md:110` |

---

## 5. 行动与恢复补充语义（`docs/R12_OPEN_WAKE.md:107-111,125`）

这些是"行动与恢复"正文中的补充声明，不属于 E/Q 编号，但同属 R12-2c-final 验收范围。

| 语义验收点 | 现有测试 / 代码 | 强度 | 说明 |
| --- | --- | --- | --- |
| S1 新来源核验按引用顺序读取完整必要证据；v33 `verified_claims_hash` 保存声明+证据联合摘要；测试辅助签发入口位于测试代码 | `tst_memoryagent.cpp` `actionEvidenceBinding(normal)` L3371 + `verifierInputBudgetKeepsWholeEvidenceItems()` L2019；`tests/testactionverification.h:1-59`（`markTestActionProposalVerified`）；`tst_memorymanager.cpp` `v33MigratesFromV32WithVerifiedBindings()` L5851（断言新列 L5871-5877） | explicit | 联合摘要列存在且旧行取 NULL（L5888-5893）；"预算不足不发 verifier"见 E03 |
| S2 v32 遗留、只有声明摘要的旧 verified 待发提案会关闭 | 代码：`src/memory/memorymanager.cpp:11687-11700`（无 `verified_payload_hash`/`verified_claims_hash`/`verified_request_id` → 置 withheld，`verified_binding_missing`）；测试仅 `v33MigratesFromV32WithVerifiedBindings()` L5851 构造旧行并断言新列为 NULL | **generic** | 前置数据被构造，但"提交时被 withheld/诊断码 `verified_binding_missing`"的断言**没有**；无专门用例 |
| S3 设置热更新/启动时发现权限收缩推进 action epoch；恢复权限不复活旧 epoch 的旧行动 | 代码：`src/app/appcontroller.cpp:73-104`（`reconcileActionPermissions`，启动 L532 与设置保存 L800 调用）；epoch 拒绝机制在 `verifiedProposalExpiresAfterOwnerPrivateInput()` L1691（A08）、`lateVerifierResultAfterEpochBumpCannotPromote()` L3864、`actionContextEpochPersistsAndBumps()` L5970 泛化覆盖 | **unrecovered** | `AppController` 未纳入任何测试目标（`tests/CMakeLists.txt` 无 `appcontroller.cpp`），权限收缩→bump 的接线无用例；只有 MemoryManager 层 bump/stale 语义被泛化覆盖 |
| S4 QQ 动作在逐句等待后复核权限，Outbox 管理唯一重试预算 | `tst_actionoutbox.cpp` `qqOutboxProtocolBoundary()` L490（等待后 `validateOutboxActionBeforeSend`，见 L581-585、L591-620）；`tst_qqdelivery.cpp` `textPacingAndSlowAcknowledgement()` L12、`orderedSentences()` L52（单次重试预算/重复回执不推进） | explicit | 发送前复验 + 队列层唯一重试 |
| S5 已委托提醒按自身状态与有效期复核 | 代码：`src/memory/memorymanager.cpp:9909-9911`（`ReminderDelivery` 发送前校验 `state='dispatching'`、`due<=now`、`expires>now`）；测试：`tst_actionoutbox.cpp` `reminderDeliveryOnlyOnce()` L870、`tst_memorymanager.cpp` `outboxReminderAndShareStateMachine()` L5525、`explicitTimeDelegationRejectedAndReminderRemains()` L4642 | **generic** | 状态机/幂等/不再直发均有显式覆盖，但"提醒已入 Outbox 后到期/状态变化"的边界没有被专门构造断言 |
| S6 关注对外动作被拒时内部记录与修订仍完成；原关注等待新的依据；取消单纯拒绝产生的定时领取 | `tst_memorymanager.cpp` `contactNowRequiresBasis()` L4267（被拒按 keep 放回队列，不无限 defer）、`contactNowRejectsFabricatedEvidence()` L4308、`reflectionContactAlsoRequiresBasis()` L4571（不写 reflection_contact，保留 consider）、`contactRejectedDoesNotEmitProposed()` L4793、`contactBasisRevalidatedOnClaim()` L4685（TOCTOU 领取前重验） | explicit | 修订流水 `contact_rejected` 有断言（L4564-4567、L4787-4789） |

---

## 6. 真实模型项与数据库项（单列）

### 6.1 真实模型验收（依赖 API key / 网络）

证据：`build/stage-b/r12-acceptance-final/r12-matrix.log`（22:43:32，`Totals: 15 passed, 0 failed`，只运行了 4 个函数/数据行 = 8+1+3+1，加 init/cleanup 恰为 15）；`build/stage-b/r12-real/r12-real-model.log`（22:31:04，`Totals: 5 passed`）；`build/stage-b/r12-real-final/r12-matrix.log`（22:34:10，15/15）。矩阵日志内 verifier 记录为 `provider=deepseek model=deepseek-v4-flash http=200`，`latency_ms≈1093–2570`，真实模型场景记录到真实 usage（`prompt_tokens=722/614/637` 等；本地 mock 场景为 `-`），**可证真实网络调用**。

| 真实模型验收项 | 证据（测试/日志） | 依赖 | 树一致时能否免重跑 |
| --- | --- | --- | --- |
| OpenWake 真实模型主动联系：核验 1 次 → 交给 mock sender 1 次 → 写回历史 | `tst_memoryagent.cpp` `realActionAcceptance(A-contact)`（`realActionAcceptance_data` L3222、函数 L3230、凭据 L3232-3234、mock sender L3309-3325）；矩阵日志 A-contact PASS，`realActionAcceptance` 共 2 次 Semantic request（A + D） | 需要真实：`MAKO_R12_ACCEPTANCE_KEY`/`MAKO_R12_ACCEPTANCE_URL` + DeepSeek 网络；不依赖真实 QQ/Pet（投递为本地 mock sender） | 若已接受 22:43:32 矩阵日志（15/15）且树一致，可免重跑；但真实模型非确定（见 §8 与 §10-U9） |
| 内部成果分享：核验 1 次 → mock sender 1 次 → 写回历史 | `realActionAcceptance(D-share)` 同上；矩阵日志 D-share PASS，活动（`activityByRef`）状态 `completed`、Outbox=1、`sends=1` | 同上 | 同上 |
| 休息场景：完成且发送零次 | `realActionAcceptance(B-quiet)`：断言 `open_wake.state='committed'`、`action_outbox=0`、`review_failure=0`（L3298-3303）；矩阵日志 B-quiet PASS 且该函数 0 次 verifier 请求 | 同上（要真实模型产出 rest/quiet） | 同上 |
| 真实 verifier 拒绝"有合法引用但缺乏文本依据"的声明 | `realVerifierRejectsUnsupportedClaim()` L3328（凭据 L3330-3332）；矩阵日志 PASS，`diagnostic=verifier_insufficient` | 需要真实 key/网络 | 同上 |

本地 mock 可替代、**不需要 key** 的项：E01–E08 全部（`DeferredCompletionServer` 本机 HTTP mock）、V01–V14/校验收敛（本机 mock 或纯函数）、Q01–Q05（本机 WebSocket）。

### 6.2 数据库 / 迁移项

| 数据库验收项 | 证据 | 本地可复验性 | 树一致时能否免重跑 |
| --- | --- | --- | --- |
| 真实 v32 备份副本升级到 v33 | 应用日志 `build/stage-b/r12-acceptance-final/migration/v32/app_data/logs/logs_20261127_224400_570_be7d160caf4c.txt`：`当前数据库版本: 32` → 加 7 列 → `数据库结构升级完成,版本: 33`；该副本 DB 头部 `user_version=33`（SQLite 头偏移 60 大端，本轮只读复核）；`build/stage-b/Windows_AI.exe` 与 `migration/v32/Windows_AI.exe` 均为 109710968 字节（22:38:06 构建） | 迁移逻辑由 `tst_memorymanager.cpp` `v33MigratesFromV32WithVerifiedBindings()` L5851、`v33MigrationFailureRollsBackAllDdl()` L5928、`v32MigrationReplaysIdempotently()` L5769 覆盖；真实副本升级不可在测试内复现 | 若接受该日志与 DB 头部证据，可免重跑；重跑需再次准备 v32 副本 |
| v33 备份恢复副本可读 | 日志 `migration/v33/app_data/logs/logs_20261127_224401_078_d8754cc041d4.txt`：打开即 `当前数据库版本: 33`，无迁移动作；DB 头 `user_version=33` | 同上（`databaseBackupIsConsistent()` L2401 覆盖备份副本行数/版本，但只针对新库） | 可免重跑（日志+头部已佐证） |
| 两份副本完整性、外键、关键表行数检查一致 | **仓库内无该检查的原始输出**（全仓检索 `integrity_check`/`foreign_key_check`/`quick_check` 无 R12 记录） | 本地测试仅覆盖备份行数/版本（`databaseBackupIsConsistent()` L2401），没有完整性/FK 检查 | **不能免**：若验收要求该项证据，需重新执行检查并归档输出 |
| 生产库 v32→v33 迁移 | `docs/R12_OPEN_WAKE.md:15`："生产库仍 v32，迁移待独立评审"——未执行 | 不在本轮自动测试范围 | 不适用（未做，保持待办） |

---

## 7. 23 个测试目标的用例统计

- 目标清单与编号来自 `tests/CMakeLists.txt`（HEAD 下 23 个）及 `build/stage-b/r12-at-most-once-ctest.log` 的 `1/23 … 23/23` 编号。
- "测试函数数"= 当前测试二进制 `-functions` 实际列出的测试函数个数（**不含** `*_data` 数据驱动函数、不含 `initTestCase`/`cleanupTestCase`）。本表数字由本轮对现有二进制的只读调用获得（工作目录放在 `%TEMP%`，未写入仓库）。
- "全量 Totals"= 归档日志中的整目标 `Totals:` 行；没有归档的写 `unknown`，不编造。按 QTest 规则，Totals 会把数据行另计，故与函数数不同口径，本表不混算。

| ctest # | 目标 | 测试类 | 测试函数数 | 归档全量 Totals | 最近通过证据（日志） | 备注 |
| ---: | --- | --- | ---: | --- | --- | --- |
| 1 | `tst_runtimelog` | `TestRuntimeLog` | 4 | unknown | `r12-at-most-once-ctest.log` Passed 0.23s | — |
| 2 | `tst_memorymanager` | `TestMemoryManager` | 154 | **157 passed, 0 failed, 1 skipped**（`r12-recovery-tests/storage-suite.log`，22:36:27；其中 `seedRealDatabaseCopy` 因 `MAKO_REAL_DB` 未设 skipped） | `r12-at-most-once-ctest.log` Passed 40.06s（22:40，最终源码） | V06/V09/退避/公平/恢复/迁移主力；storage-suite 所用二进制 22:35:24 略早于最终 src 22:37:27，总量仅作参考 |
| 3 | `tst_configmanager` | `TestConfigManager` | 15 | unknown | `r12-at-most-once-ctest.log` Passed 0.06s | 22:29 曾失败，后续构建修复（见 §8） |
| 4 | `tst_memorypolicy` | `TestMemoryPolicy` | 18 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 5 | `tst_recentcontext` | `TestRecentContext` | 9 | unknown | `r12-at-most-once-ctest.log` Passed 0.03s | — |
| 6 | `tst_memorytextutils` | `TestMemoryTextUtils` | 6 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 7 | `tst_memoryaccesspolicy` | `TestMemoryAccessPolicy` | 6 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 8 | `tst_groupmembership` | `TestGroupMembership` | 1 | unknown | `r12-at-most-once-ctest.log` Passed 0.08s | 链接 WebSockets/Network |
| 9 | `tst_memoryagent` | `TestMemoryAgent` | 85 | unknown（全量 Totals 未归档；过滤矩阵 15/15、真实模型子集 5/5 见 §6） | `r12-last-memoryagent-ctest.log` 1/1 Passed 24.57s（22:43:56，HEAD 最终二进制）；`r12-at-most-once-ctest.log` Passed 26.37s | E/Q/V 主力；真实模型用例默认 `QSKIP`，需环境变量才跑 |
| 10 | `tst_interactionsegmenter` | `TestInteractionSegmenter` | 4 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 11 | `tst_memoryoutputguard` | `TestMemoryOutputGuard` | 8 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 12 | `tst_sessionrouter` | `TestSessionRouter` | 5 | unknown | `r12-at-most-once-ctest.log` Passed 0.17s | — |
| 13 | `tst_promptassembler` | `TestPromptAssembler` | 16 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 14 | `tst_qqreplypolicy` | `TestQQReplyPolicy` | 9 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 15 | `tst_qqmessageparser` | `TestQQMessageParser` | 7 | unknown | `r12-at-most-once-ctest.log` Passed 0.02s | — |
| 16 | `tst_globalmoodstate` | `TestGlobalMoodState` | 5 | unknown | `r12-at-most-once-ctest.log` Passed 0.03s | — |
| 17 | `tst_ttslifecycle` | `TestTtsLifecycle` | 6 | unknown | `r12-at-most-once-ctest.log` Passed 13.37s | Network；耗时主要来自 TTS 时序 |
| 18 | `tst_qqdelivery` | `TestQQDelivery` | 3 | unknown | `r12-at-most-once-ctest.log` Passed 25.31s | 本机 WebSocket 真实帧，无外网 |
| 19 | `tst_settingspages` | `TestSettingsPages` | 1 | unknown | `r12-at-most-once-ctest.log` Passed 0.13s | `QT_QPA_PLATFORM=offscreen`（CMake ENVIRONMENT） |
| 20 | `tst_concern` | `TestConcern` | 8 | unknown | `r12-at-most-once-ctest.log` Passed 0.40s | — |
| 21 | `tst_cognitivesnapshot` | `TestCognitiveSnapshot` | 13 | unknown | `r12-at-most-once-ctest.log` Passed 0.74s | — |
| 22 | `tst_actionoutbox` | `TestActionOutbox` | 37 | unknown（过滤子集 7/7 见 `r12-recovery-tests/outbox.log`；A22 单测 3/3 见 `r12-share-restart/r12-share-restart.log`） | `r12-at-most-once-ctest.log` Passed 10.82s | Q01–Q05、A07–A23、Outbox 状态机 |
| 23 | `tst_actionpolicy` | `TestActionPolicy` | 17 | unknown | `r12-at-most-once-ctest.log` Passed 0.66s | `QTEST_MAIN`（GUI 平台）；V12–V14/身份/证据用途 |

合计测试函数数：437（23 个目标之和；仅为函数级口径，非 QTest Totals）。
> 说明：`docs/AGENT_WORKLOG.md:520` 记录了 R12-2c 阶段的 `tst_memorymanager 154/1 skipped`、`tst_memoryagent 100`、`tst_actionoutbox 38`、`tst_actionpolicy 18`、`tst_cognitivesnapshot 15`。这些是**当时阶段**的 Totals 口径数字，且 `tst_memoryagent` 在 HEAD 前又增删了用例；本轮未全量运行这些目标，故不把它们当作本轮可复核的通过数。

---

## 8. 证据链（ctest / 矩阵 / 时间线）

| 时间 | 证据文件 | 实际输出 | 解释 |
| --- | --- | --- | --- |
| 22:29:05 | `build/stage-b/r12-final-ctest.log` | 21/23（`tst_memorymanager`、`tst_configmanager` Failed） | 早期构建的失败；随后 22:30–22:38 的修复构建覆盖，**不是最终状态** |
| 22:32:51 | `build/stage-b/r12-acceptance-ctest.log` | 100% 23/23 | 修复后首次全量通过 |
| 22:35:42 | `build/stage-b/r12-verified-ctest.log` | 100% 23/23 | 再次全量通过 |
| 22:36:27 | `build/stage-b/r12-recovery-tests/storage-suite.log` | tst_memorymanager 全量 `157 passed, 0 failed, 1 skipped` | V/C/R/D 相关 memorymanager 用例逐条 PASS 证据 |
| 22:40:07 | `build/stage-b/r12-at-most-once-ctest.log` | **100% 23/23**（118.64s） | HEAD 前最终全量回归；关键目标二进制 22:37:49–22:38:06 构建自 src mtime 22:37:27（未受影响的小目标仍为 22:24 构建） |
| 22:43:32 | `build/stage-b/r12-acceptance-final/r12-matrix.log` | `Totals: 15 passed, 0 failed`（过滤矩阵：actionEvidenceBinding 8 行 + openWakePersistConflict + realActionAcceptance 3 行 + realVerifierRejectsUnsupportedClaim） | E01–E08 + V 系列 + 真实模型验收的最终通过证据（GBK 编码） |
| 22:43:56 | `build/stage-b/r12-last-memoryagent-ctest.log`、`Testing/Temporary/LastTest.log` | tst_memoryagent 1/1 Passed 24.57s | HEAD 最终 `tests/tst_memoryagent.cpp` 重建后的复跑 |
| 22:44:00–22:44:01 | `build/stage-b/r12-acceptance-final/migration/` 下 `v32`、`v33` 两个副本的 `app_data/logs/*.txt` | `32 → 33` 迁移完成；v33 副本打开即 33 | 数据库副本验收的真实应用日志 |

补充：`Testing/Temporary/LastTestsFailed.log` 仍保留 22:29 的 `2:tst_memorymanager / 3:tst_configmanager` 两行，时间戳为 22:29，**是历史残留**，不代表当前树失败。

`build/stage-b/r12-controlled/r12-matrix.log`（22:41:09）与 `r12-e417073/r12-matrix.log`（22:38:49）为 14 passed/1 failed：失败项均为 `realActionAcceptance(A-contact)`；最终 22:43 的日志已 15/15，原因见 §10-U9。

---

## 9. 可复核命令（只读示例）

以下命令用于复核 §7 的统计表与本文证据；未在本次生成中运行全量（ctest 会写 `build/stage-b/Testing/`，按任务约束未改动 build）。

```powershell
cd D:\QtProject\Windows_AI
git rev-parse HEAD                 # 期望 4dcafb6ffbcf438fde489399bd19315814dbb267
git rev-parse 'HEAD^{tree}'        # 期望 c239b5e75228c5760d9321a96a603648216c7fe8

# 让测试 exe 找到 Qt/MinGW 运行库（路径取自 build/stage-b/CMakeCache.txt）
$env:PATH = "D:/QT/6.11.1/mingw_64/bin;D:/QT/Tools/mingw1310_64/bin;" + $env:PATH
Set-Location $env:TEMP

# 列目标（23 个）与单目标用例数
ctest --test-dir D:\QtProject\Windows_AI\build\stage-b -N
& "D:\QtProject\Windows_AI\build\stage-b\tests\tst_actionpolicy.exe" -functions
& "D:\QtProject\Windows_AI\build\stage-b\tests\memory-agent\tst_memoryagent.exe" -functions

# 全量回归（会写 build/stage-b/Testing；仅在允许写 build 时执行）
ctest --test-dir D:\QtProject\Windows_AI\build\stage-b --output-on-failure

# 真实模型矩阵（需凭据；本文件未执行，仅记录原验收用的筛选方式）
$env:MAKO_R12_ACCEPTANCE_KEY = "<key>"
$env:MAKO_R12_ACCEPTANCE_URL = "<url>"
& "D:\QtProject\Windows_AI\build\stage-b\tests\memory-agent\tst_memoryagent.exe" `
    actionEvidenceBinding openWakePersistConflictDoesNotPromoteOtherContent `
    realActionAcceptance realVerifierRejectsUnsupportedClaim
# 期望 Totals: 15 passed（8 行 + 1 + 3 行 + 1 + init/cleanup）

# 读取 GBK 日志（直接 Get-Content 会乱码）
$enc = [System.Text.Encoding]::GetEncoding(936)
$enc.GetString([System.IO.File]::ReadAllBytes("D:\QtProject\Windows_AI\build\stage-b\r12-acceptance-final\r12-matrix.log")) -split "`r?`n" |
    Where-Object { $_ -match '^(PASS|FAIL|Totals|QINFO.*\[Semantic\])' } | Select-Object -Last 40

# 只读复核迁移副本 SQLite user_version（偏移 60，大端 4 字节）
foreach ($p in @(
  "D:\QtProject\Windows_AI\build\stage-b\r12-acceptance-final\migration\v32\app_data\memory\QianDaoMoZi_memory.db",
  "D:\QtProject\Windows_AI\build\stage-b\r12-acceptance-final\migration\v33\app_data\memory\QianDaoMoZi_memory.db"
)) {
  $fs = [System.IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
  $buf = New-Object byte[] 100; $null = $fs.Read($buf, 0, 100); $fs.Close()
  "$p => user_version=$([BitConverter]::ToInt32($buf,60)) (0x21000000 表示大端 33)"
}
```

---

## 10. 不可恢复 / 未覆盖清单与原因

| # | 项目 | 状态 | 原因与影响 |
| ---: | --- | --- | --- |
| U1 | **V/C/R/D 原始编号清单与报告原文** | **不可恢复** | `docs/` 从未提交任何 R12-2C 报告文件，全仓/ git 历史检索均无；`docs/R12_OPEN_WAKE.md:121` 只有一句汇总，且自述"已回退的旧编号用例缺少原始清单"。C/R/D 三个字母的原编号与含义完全无法还原；同轮独立核对 `docs/R12_2C_FINAL_BASELINE.md` 结论相同。本文只能按语义分组映射（§4） |
| U2 | E05–E08 与具体用例的一一编号对应 | 不可恢复（按语义推断） | 原表只给"E05–E08"块描述与 6 句行为描述，未给逐号清单；本文按"回包前/核验后/提交后 + 绑定校验"映射并明确标注推断 |
| U3 | 原验收执行命令/环境/报告 SHA 的原始记录 | 未入库 | 矩阵日志无命令行头，未记录 `MAKO_R12_ACCEPTANCE_*` 取值；只能由 `Totals=15`、函数筛选结果反推；原报告 SHA `a9289c2` 与当前 HEAD 同 tree（`c239b5e7…`），已在 `docs/R12_2C_FINAL_BASELINE.md` 考证 |
| U4 | 权限收缩推进 action epoch / 热更新接线（S3） | **unrecovered** | `AppController::reconcileActionPermissions`（`src/app/appcontroller.cpp:73-104`，启动 L532、设置保存 L800 调用）未被任何测试目标链接（`tests/CMakeLists.txt` 不含 `appcontroller.cpp`）；只有 MemoryManager 的 bump/stale_origin 泛化语义被覆盖 |
| U5 | v32 遗留无指纹 verified 行的关闭断言（S2） | generic | `src/memory/memorymanager.cpp:11687-11700` 实现 `verified_binding_missing` → withheld，但无测试断言该路径；迁移用例只断言旧行新列为 NULL、state 仍 `verified` |
| U6 | 提醒"入 Outbox 后过期/状态变化"边界（S5） | generic | 发送前 SQL 已校验 `expires>now`（`src/memory/memorymanager.cpp:9909-9911`），但无用例构造"提交后过期"边界 |
| U7 | **真实 QQ / Pet 现场投递** | **未做（不可由自动测试替代）** | 全部 Q01–Q05 为本机 `QWebSocketServer` 的真实协议帧；真实模型验收的投递由本地 mock sender 接收。无真实 QQ 账号送达、无真实 Pet UI 投递证据 |
| U8 | 数据库完整性/外键/关键表行数"检查一致"的原始输出 | 未入库 | 全仓检索无 `integrity_check`/`foreign_key_check`/`quick_check` 的 R12 记录；本轮只能只读复核两份副本 SQLite 头 `user_version=33` 与迁移日志，不能复核当时的完整性/FK/行数结论。若该项必须作为证据，需重新执行并归档 |
| U9 | 真实模型验收的确定性 | 有限 | `A-contact` 在 22:38:49（`r12-e417073`）与 22:41:09（`r12-controlled`）两次失败：模型选择 `start_activity` 而非 `contact`，断言 `action_outbox 0 != 1`；最终通过依赖 HEAD 对测试配置的调整（关闭非分享场景 `private_writing`、补一条主人历史轮次）。真实模型输出仍非确定，**重跑不保证稳定** |
| U10 | 生产库 v32→v33 迁移 | 未执行 | `docs/R12_OPEN_WAKE.md:15` 明确"生产库仍 v32，迁移待独立评审"；本轮只验证了备份副本 |
| U11 | 工作区新出现的第 24 个测试目标 `tst_cognitivecontext`（R12-3a） | 未纳入 | `tests/CMakeLists.txt` 当前未提交改动新增该目标；它不属于 HEAD 的 23 目标，也不在本文任何证据范围内；重建后目标数将变为 24，`23/23` 不再是当前工作区的正确口径 |

---

## 11. 结论

### 11.1 树一致（`HEAD=4dcafb6`，`tree=c239b5e7…`，除本文件外工作区干净）时可免重跑的自动复验范围

- **E01–E08**：`tst_memoryagent::actionEvidenceBinding` 8 行 + `openWakePersistConflictDoesNotPromoteOtherContent`，本机 mock，无 key/网络/真实发送，可直接复验；最终矩阵日志 `Totals: 15 passed` 已覆盖该函数集。
- **Q01–Q05**：`tst_actionoutbox::qqOutboxProtocolBoundary` 4 行，本机 WebSocket 真实协议帧，无外网，可直接复验；`r12-recovery-tests/outbox.log` 7/7 与 23/23 全量 ctest 覆盖。
- **V/C/R/D 语义组**：§4 的 44 条全部由 `tst_memoryagent` / `tst_memorymanager` / `tst_actionoutbox` / `tst_actionpolicy` 覆盖，其中 tst_memorymanager 逐条 PASS 见 `storage-suite.log`（157 passed/1 skipped）；整目标通过见 23/23。
- **行动与恢复补充**：S1、S4、S6 显式覆盖可直接复验；S2/S5 只能复核代码路径与泛化用例（generic）。
- **数据库迁移逻辑**：`v33MigratesFromV32WithVerifiedBindings` 等 4 个迁移用例可在临时库上复验（不触碰真实库）。

### 11.2 仍需真实运行或额外证据的范围

1. **真实模型验收**（4 项）：依赖 `MAKO_R12_ACCEPTANCE_KEY/URL` 与 DeepSeek 网络；22:43:32 矩阵日志 + 22:31 真实模型日志为已有通过证据，若接受该证据可免重跑；但真实模型非确定（U9），重跑需重新承担不稳定风险。
2. **数据库副本"完整性/外键/行数一致"**（U8）：原始输出未入库，不能凭现有日志替代；如验收要求该证据，必须重做检查并归档。
3. **真实 QQ/Pet 现场投递**（U7）：现有自动测试只能证明"到本机协议帧/mock sender 为止"；真实端到端送达未做，也不可由现有用例替代。
4. **权限收缩 → epoch 推进的应用层接线**（U4）：无自动覆盖，只能代码审阅或补测试；`恢复权限不复活旧行动` 也只能由 MemoryManager 层泛化用例间接支持。
5. **生产库迁移**（U10）：未执行，不在自动测试可复验范围。
6. 若工作区保留 R12-3a 改动（U11）：必须先从 `4dcafb6` 的干净检出重建，否则 §8 的时间线与 `23/23` 口径不再成立。

### 11.3 关键判定

> R12-2c-final 的**自动化可复验部分**（E01–E08、Q01–Q05、V/C/R/D 语义组、行动/恢复的显式用例、迁移逻辑）在 HEAD `4dcafb6` 树上具有完整且可重复的证据，且最近的全量 ctest 为 **23/23 通过**；**不可自动复验的部分**是：V/C/R/D 原始编号清单与报告原文（不可恢复）、真实 QQ/Pet 现场投递（未做）、数据库完整性/FK/行数检查原文（未归档）、权限收缩应用层接线（无测试）、真实模型重跑的确定性（受模型随机性限制）。

---

## 12. 后续更新（R12-3a 落地后补记）

- R12-3a 第一切片已提交，新增 `tst_cognitivecontext`（14 个测试函数，`16 passed`）与
  `tst_memoryagent::cognitiveShadowSharesIdentityAcrossEntries`，测试目标从 23 个变为 24 个；
  本轮全量 ctest 为 **24/24**（不是 §8 所述 23/23 口径）。
- 本文件 §8/§11 的 23/23 结论仍对应 R12-2c-final 基线树 `4dcafb6`（目录中除本文件外干净时）；
  当前工作区已含 R12-3a 提交，若需重建 R12-2c-final 验收，请从 `4dcafb6` 干净检出。
