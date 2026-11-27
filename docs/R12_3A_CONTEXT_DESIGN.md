# R12-3a 统一认知上下文与证据目录（第一切片）

- 基线：`main = 4dcafb6ffbcf438fde489399bd19315814dbb267`（tree `c239b5e75228c5760d9321a96a603648216c7fe8`）
- 范围：源码盘点（见 `R12_3A_SOURCE_INVENTORY.md`）、共享 `CognitiveContext` 数据层、证据身份与去重、
  时间/错误状态统一、shadow 对照实现与测试。
- 本切片**不替换生产 prompt**、**不新增数据库 schema**、**不删除** `CURRENT_UNDERSTANDING` / `MEMORY_DATA`
  / 认知快照 / 旧历史模块、不发起任何额外模型请求。

---

## 0. 结论摘要

1. 新增 `src/cognition/cognitivecontext.{h,cpp}`：统一证据身份（stable_ref + version + facet）、
   语义用途、四条时间线、来源覆盖状态、隐私投影、预算/重复度报告，以及新旧上下文对照
   `ShadowComparison`。
2. 复用既有组件而不是重造：证据材料来自 `CognitiveSnapshotBuilder` 的输出与统计，
   MEMORY_DATA claims 二次走 `MemoryAccessPolicy`，检索证据沿用 `MemoryEvidence`
   （`chat/<turn>:user|assistant` 等既有 ref），matter 认识沿用 `revision/<matter>/<version>`
   与 `currentUnderstanding` 的 `source/<id>`，工具结果沿用 `MemoryAgentItem` 的
   内部 id → stable_ref 映射。
3. Shadow 只观测：开关默认关闭（环境变量 `MAKO_COGNITIVE_SHADOW=1` 或
   `LLMService::setCognitiveShadowEnabled(true)` 测试/诊断用）。打开后只把**本路径已经读取到**的
   材料传给 builder，纯内存归一化；不额外查库、不发请求、不写库、不改变本轮 `messages`。
4. 第一切片 8 条验收条件全部由自动测试覆盖（`tst_cognitivecontext` 14 个测试函数 +
   `tst_memoryagent::cognitiveShadowSharesIdentityAcrossEntries` 跨入口实测）。
5. 未完成/未验证项如实列在 §7：生产库/真实模型 shadow 样本、工具轮/consult/核验入口的运行时 shadow、
   回执材料读取、群聊/第三方投影的真实数据样本。

---

## 1. 现有上下文结构（旧）

资料流全景与逐项说明见 `R12_3A_SOURCE_INVENTORY.md`，这里只列与统一上下文直接相关的结构：

| 入口 | 当前材料 | 结构问题 |
|---|---|---|
| 前台 `prepareReply` | system prompt、CURRENT_UNDERSTANDING、concern、activity、能力清单、认知快照、短期历史、身份/情境/时间、检索状态、RECENT_GROUP_CONTEXT、MEMORY_DATA、跨渠道私密证据、工具结果、当前输入 | 同一 chat 轮次会以 `chat/<id>`、`reply/<id>`、`chat/<id>:user`、`chat/<id>:assistant`、工具内部 id 等多种拼写重复序列化；每类各自过滤/裁剪；失败与空结果语义不一致 |
| 私下回顾 `generatePrivateReview` | 人格、认知快照、回顾协议、指令、thread（来源/历史/prior/concern/basis/time_context/consulted） | thread 是 ad-hoc JSON；来源轮次在快照与 thread 中各出现一次；无统一 evidence 目录 |
| matter consult 第二轮 | `consulted` 行（turn/text/reported_at）+ 原 thread | consult 轮次与短期历史同源但身份未共享 |
| memory_recall / memory_read | `MemoryAgentItem`（`recent:<eventId>` 等 run 内句柄） | 句柄不是 stable_ref；与快照/历史无法在数据层对上 |
| 行动核验 `verifyActionProposal` | 服务端重读的完整证据项（claim/source） | 与模型上下文是两套身份/摘要；`payload_digest` 只覆盖声明与证据摘要 |
| Outbox/Reminder | `delivery_ledger`/`action_outbox`/domain 行 | 模型上下文看不到稳定回执引用；只能通过 claims 间接引用 |

旧结构的关键事实：`m_run.evidence` / thread refs 是“按入口临时拼出的引用表”，不是统一目录；
同一原始事件是否重复、版本是否最新、失败是否被当成空历史，都没有统一可审计表示。

---

## 2. 新结构：`CognitiveContext`（shadow 数据层）

代码：`src/cognition/cognitivecontext.h`（结构定义）、`cognitivecontext.cpp`（归一化/去重/比较）。

### 2.1 证据身份

```text
EvidenceIdentity = { sourceKind, ref, version, facet }
eventKey()  = sourceKind|ref@version      ← “同一原始事件”跨模块识别
stableKey() = eventKey#facet              ← 同一事件的不同语义面
```

归一化规则（`evidenceIdentityForRef()`，`cognitivecontext.cpp:193`）：

| 既有拼写 | 统一身份 |
|---|---|
| `reply/<turn>`、`chat/<turn>:assistant` | `chat` / `chat/<turn>` / facet `assistant_expression` |
| `chat/<turn>:user`、`chat/<turn>`（历史 user 面） | `chat` / `chat/<turn>` / facet `user_statement` |
| `revision/<matter>/<v>` | `matter` / `matter/<matter>` / version `v`（与 `evidenceIdentityForMatter` 同 key） |
| `episode/<id>` / `interaction/<id>` / `long_summary/<id>` | `episodic` / `interaction_summary` / `summary` |
| `source/<id>` | `source/<id>`（原始认知来源，与事项认识区分） |
| `reflection/<turn>`、`concern/<root>`、`thought|activity/<id>`、`wake/<id>`、`reminder/<id>`、`delivery/<key>` | 原样，version 由材料显式给出 |
| `chunk/<id>`、`recent:<id>` 等 run 内句柄 | 标记 `stable=false`，只作诊断/对照，不算 stable_ref |

规则：
- 不同版本是不同身份（matter revision 1/2/3 共存，`crossVersionGroups` 显式统计）。
- 派生摘要/认识/推断与原始证据是不同身份，用 `derivedFrom` 关联；不因文本相关或相等合并。
- 不同日期的相同文本因 ref 不同而保持区分。
- 同一身份在多个入口重复出现时只保留一份文本，`usages` 记录所有入口（去重依据是身份，不是文本）。

### 2.2 语义与权威

`EvidenceKind`（语义用途）与既有 `EvidenceAuthority`（谁能证明什么）分开：
UserStatement / ExternalObservation / AssistantExpression / DerivedSummary / SelfInference /
InternalAutonomousRecord / UserDelegation / ExecutionResult / ActivityArtifact。
authority 继续复用 `cognitivesnapshot.h` 的六域定义，没有新造一套权威域。

### 2.3 时间语义

`EvidenceTime`：reportedAt / eventAt / effectiveAt / recordedAt 四条时间线，未知保持未知。
`CognitiveContextTime` 由服务端按本次构建的 `now` 提供 local/UTC/timezone 与
`relativeLabel()`（今天/昨天/N 天前 + 绝对时间）。文本含“今天/明天”但事件时间未锚定时，
`relativeExpressionUnanchored=true`，绝不继承旧消息里的相对时间。

### 2.4 覆盖与错误状态

`CognitiveCoverageReport` 区分：`complete / empty / partial / degraded / denied /
error / budget_trimmed / not_queried`，每个来源一行（snapshot、current_understanding、
memory_data、history、retrieval、recent_group_context、private_evidence、memory_tool、
matter_consult、review_thread、review_retrieval、execution_receipt、reminder、concern、
activity、current_input…）。检索 `degraded/error` 不会被解释成空历史；
“本轮没有读取”（not_queried）与“读到了确实为空”（empty）分开。

### 2.5 隐私投影

同一目录、按入口投影（`applyProjection()`，`cognitivecontext.cpp:1748`）：
- 只有主人私聊/桌宠入口（owner private，且非盲区）携带 owner 私有域
  （motivations/commitments/activities/recent_activity_results/internal_wakes、
  concern/activity/reminder/私密证据/回顾 thread）。
- 群聊/第三方入口：`privacy=private`/sensitive 证据、owner 私有域、其它私聊会话来源一律过滤，
  过滤数量与原因纳入 `ShadowComparison`；过滤后若仍有 private 证据计为 leak（第一切片实测 0）。
- MEMORY_DATA claims 在 builder 内再走一次 `MemoryAccessPolicy::evaluate`：材料若绕过入口过滤
  会在统一目录里被拒绝，不会因“统一上下文”扩权。

### 2.6 预算与重复度

`CognitiveBudgetReport` 记录：统一上下文紧凑 JSON 字节/字符、估算 token（ceil(chars/2)，
CJK 粗略，不作为 provider 精确值）、证据文本字节、材料总字节、唯一证据字节、
重复材料字节、当前实际注入 messages 字节、数据消息字节、按用途域字节、
证据数/唯一事件数。`snapshot_budget_bytes` 继续取自 `CognitiveSnapshotBuilder::promptByteBudget()`
（生产配置 `cognition.snapshot_budget_bytes=4096`，本切片未改），只约束快照投影，
**不当作统一上下文总预算**，也不在本切片裁剪生产 prompt。

### 2.7 新旧对照

`ShadowComparison`：旧 evidence 目录 vs 统一目录的
matched/missing/extra、跨模块重复身份数、同事件多 facet 数、同 ref 多版本组数、
身份冲突数（同一 stableKey 被标成不同 authority/kind）、隐私过滤/泄漏、
相对时间未锚定数、原始证据覆盖率、run 内句柄数量。旧目录里的 `ref:N`/工具句柄
不计入 stable_ref 覆盖。

---

## 3. Shadow 接线（不替换生产输入）

代码位置：

- 开关与报告：`llmservice.h`（`setCognitiveShadowEnabled` / `lastCognitiveShadowReport` /
  `cognitiveShadowReports`）；实现 `llmservice.cpp:3682`。
- 前台：`llmservice.cpp:1392` 附近。材料全部是本路径已读取的变量：
  `CognitiveSnapshotBuilder` 输出（`cognitiveSnapshotMessage` 增加可选 out 参数，
  `llmservice.cpp:216`）、`currentUnderstanding`/concern/activity/reminder、
  `PromptAssembler::MemoryContext`、`historyQA`、`m_lastRetrievalResult`、
  `recentBundle`、`privateEvidence`、`capabilities`、`m_run.evidence`。
- 回顾：`llmservice.cpp:2041` 附近。材料是回顾快照 + thread（来源/历史/prior/concern/
  basis/consulted）+ `history_retrieval_status`；对照目录按 thread 实际注入的 refs 构造。

不变量（测试与代码共同保证）：
1. 关闭时连快照 out 参数都不传，零额外工作。
2. 打开后 builder 不查库（`buildShadow` 全部参数来自 request）、不发网络请求、不写库。
3. 生产 `messages`/`modelThread` 与旧路径逐字一致；shadow 报告只进内存对象与 INFO 日志
   （`[Cognitive][Shadow] ...`）。
4. 无 schema/配置迁移；生产 `setting.json`/`identity.json` 未改。

---

## 4. 实测对比数据（自动测试 fixture，非生产数据）

来源：`tst_memoryagent::cognitiveShadowSharesIdentityAcrossEntries`（2026-11-27 本轮构建，
本机 mock HTTP，无真实模型网络）。同一临时库：1 条 chat 轮次 + 快照 + 检索 chunk。

| 指标 | 前台 foreground_reply | 回顾 private_review |
|---|---:|---:|
| 统一证据数 | 5 | 4 |
| 唯一事件数 | 4 | 3 |
| 旧 evidence 引用数 | 4 | 2 |
| 匹配到统一身份 | 3 | 2 |
| 旧引用在统一目录缺失 | 0 | 0 |
| 跨模块重复身份数 | 2 | 1 |
| 身份冲突（错误合并） | 0 | 0 |
| 隐私过滤 / 泄漏 | 0 / 0 | 0 / 0 |
| 统一上下文字节 | 8102 | 7088 |
| 估算 token（chars/2） | 3849 | 3348 |
| 旧 messages 字节 | 11280 | 7124 |
| 覆盖状态 | partial（工具/回执等未读取） | partial（memory_data/history 等未读取） |
| 模型请求数 | 1（无 shadow 增量） | 1（无 shadow 增量） |

说明：
- 两入口对同一 `chat/<turn>` 都生成 `chat|chat/<turn>#user_statement` 身份（测试断言），
  证明“不同入口共享统一证据身份”。
- `legacy_refs=4` 中含 1 个 `ref:N` run 内编号，比较时计入 note 而不是 stable_ref。
- 该数据用于证明 shadow 管线可跑通与指标口径；生产规模/真实模型下的数据需后续运行观察
  （见 §7），本轮未启动生产应用。

单元级数据（`tst_cognitivecontext`）：5 入口归并同一 chat 身份的 usages=5；
两条同文本不同轮次事件保持 2 个身份；matter 两个 revision 共存且原始 source 不合并；
检索 degraded 与 empty 状态区分；群聊投影过滤 owner 私有域且 leak=0；
重复材料字节 = 总材料字节 − 唯一证据字节。

---

## 5. 验收条件 → 证据映射

| # | 验收条件 | 测试/证据 |
|---|---|---|
| 1 | 不同入口共享统一证据身份 | `snapshotBuilderMaterialsShareIdentity`、`cognitiveShadowSharesIdentityAcrossEntries`（前台/回顾同 chatKey） |
| 2 | 同一原始事件跨模块识别重复 | `sameEventAcrossModulesDedups`（快照/历史/检索/私密证据/工具 → 1 个 user 证据，usages=5） |
| 3 | 不同事件不会被错误合并 | `differentEventsSameTextStayDistinct`（同文本两轮 → 2 身份）；`identityConflicts=0` |
| 4 | 旧历史与当前状态保持区分 | `matterVersionsAndDerivedEvidenceDoNotMerge`（revision 2/3 共存、source 独立、derivedFrom 关联） |
| 5 | 私有资料不泄露到无权限入口 | `privacyProjectionBlocksPrivateInGroup`、`memoryDataClaimsReuseAccessPolicy`、投影 leak=0 |
| 6 | 检索失败不被解释为空历史 | `retrievalFailureIsNotEmptyHistory`（degraded/error/not_queried/empty 四态区分） |
| 7 | Shadow 不产生额外模型调用 | `cognitiveShadowSharesIdentityAcrossEntries` 断言 mock 请求数：前台 1、回顾 2（各一轮） |
| 8 | 原有行动/提醒/活动/记忆无回归 | 全量 `ctest`（本轮运行结果见 §6；24 个测试目标） |

---

## 6. 本轮验证

- 构建：`build/stage-b`（Ninja Debug，Qt 6.11.1 / MinGW 13.1）全量构建通过
  （日志 `build/stage-b/r12-3a-build.log`）。
- 新增测试目标 `tst_cognitivecontext`：14 个测试函数，`Totals: 16 passed, 0 failed, 0 skipped`
  （含 init/cleanup）。
- `tst_memoryagent` 新增 `cognitiveShadowSharesIdentityAcrossEntries`：
  `Totals: 115 passed, 0 failed, 4 skipped`（4 个 skip 是真实模型/真实库 opt-in 用例，环境变量未设置）。
- 全量 `ctest --test-dir build/stage-b`：**24/24 通过**，总耗时 132.65s
  （日志 `build/stage-b/r12-3a-ctest.log`）。
- 关键回归目标：`tst_memorymanager 157 passed / 1 skipped`、`tst_cognitivesnapshot`、
  `tst_actionoutbox`、`tst_actionpolicy`、`tst_concern`、`tst_promptassembler` 等全部通过。
- 既有测试的一处**时间相关脆弱性**修复（测试代码，非产品语义）：`actionEvidenceBinding`
  原先不固定 `quiet_hours`，在默认安静时段（23:00–08:00）运行会得到 `error:"quiet"`；
  本轮在测试 fixture 中固定为空安静时段。该问题与 R12-3a 代码无关，白天（22:43 前的
  历史回归）不会出现。
- 生产配置只读核对：`identity.json` 的 `cognition.snapshot_budget_bytes=4096`、
  `proactive.contact_daily_limit=0`、`setting.json` 均未修改；
  生产库 `user_version=33`、SHA-256 `8628AD01…`、mtime 19:38:49 未变
  （见 `R12_2C_FINAL_BASELINE.md` §6）。生产应用**未启动**、未发真实 QQ/Pet 消息、
  未跑真实模型验收。

---

## 7. 剩余风险与未覆盖（如实登记）

1. **生产规模 shadow 样本缺失**：本轮只在测试 fixture 上实测；真实库 500+ 轮、真实检索结果下
   的重复率/字节/耗时未采集。建议用户允许正常使用后，以 `MAKO_COGNITIVE_SHADOW=1` 受控观察，
   读取 `[Cognitive][Shadow]` 日志（不替换 prompt、不发消息）。
2. **工具轮/consult/核验入口的运行时 shadow 未接**：builder 已支持 `MemoryAgentItem`、
   `consultedRows`、`receipts` 材料并有单测，但 `memory_recall` 工具轮、matter consult 第二轮、
   `verifyActionProposal` 入口尚未在生产路径调用 shadow（第一切片只接前台与回顾）。
3. **真实执行回执未读库**：本切片不新增读库，`execution_receipt` 来源在 shadow 中如实标
   `not_queried`；需要后续增加只读回执查询（可复用 `delivery_ledger`/`action_outbox`，无 schema 变更）。
4. **群聊/第三方投影缺真实数据样本**：单测用合成材料验证过滤；真实群聊下 owner/第三方、
   盲区、跨渠道组合仍建议运行观察。
5. **时间相对标签的语义边界**：服务端只提供“今天/昨天/N 天前 + 绝对时间”，不做自然语言
   时间解析；旧 prompt 中的相对表达是否被模型正确理解仍需真实对话观察。
6. **token 估算**：ceil(chars/2) 只是 CJK 粗略值，不等同 provider tokenizer；预算报告不充当计费依据。
7. **旧模块仍在**：`CURRENT_UNDERSTANDING`、`MEMORY_DATA`、认知快照、recent/private evidence
   照旧注入；统一目录目前是 shadow 旁路，任何“以新替旧”的切换都必须单独评审。
8. **构建产物混入风险**：本切片在 `4dcafb6` 工作区新增代码；若日后要重建 R12-2c-final 验收，
   必须先干净检出 `4dcafb6`（见 `R12_2C_FINAL_BASELINE.md` §0-7）。

---

## 8. 后续逐模块替换建议（R12-3b 起，需单独授权）

原则：每次只替换一个入口，先让新目录覆盖旧材料，shadow 比较达到“缺失=0、冲突=0、泄漏=0、
预算可解释”后再切换；旧模块保留到对应入口验证通过。

| 顺序 | 入口 | 建议 |
|---|---|---|
| 1 | 前台 CURRENT_UNDERSTANDING | 先用统一目录生成同一份 catalog（字段/编号保持兼容），shadow 比较通过后切换；`ref:N` 目录编号与 `evidence` 字段保持现有契约 |
| 2 | 前台 MEMORY_DATA / 检索 | 统一目录按用途投影生成同一 items 形状；检索失败状态与旧 `retrievalStatus` 对齐后再切换 |
| 3 | 私下回顾 thread | 用统一目录生成 review 材料（来源/历史/prior/concern/consulted），`_origin`/schema 不动 |
| 4 | matter consult / memory_recall/memory_read | 工具结果 id 与 stable_ref 建立映射（已在 builder 有转换），先 shadow 再切换 |
| 5 | 行动核验 evidence sources | 从统一目录取完整证据项，`payload_digest`/binding 仍由服务端计算，不改核验失败闭合 |
| 6 | 执行回执 | 增加只读回执查询（无 schema），统一为 `ExecutionResult` 身份；Outbox 发送前复验逻辑不变 |
| 7 | 删除旧模块 | 只有 1–6 全部切换且全量回归/真实运行观察通过后，另行评审删除；本切片禁止删除 |

每步的前置条件：`tst_cognitivecontext` + 对应入口测试 + 全量 ctest 通过；shadow 指标
`missing_in_unified=0`、`identity_conflicts=0`、`privacy_leaks=0`；预算变化有解释；
不新增模型请求；不扩大读取权限。

---

## 9. R12-3a 第二切片：实际模型输入接管（2026-11-28 提交）

本节记录第二切片：把统一证据目录**实际投影进模型输入**（默认关闭、可回退），
不是只做 shadow。基线：`f0824d2`（第一切片）之上。

### 9.1 实际接管的路径（替换了哪些旧调用路径）

| 入口 | 原路径 | 接管后 |
|---|---|---|
| 前台 `prepareReply` | `cognitiveSnapshotMessage()` 旧快照 JSON 直接进 messages；`privateEvidence` 原样 JSON 进“跨渠道记忆资料”消息 | 用 `projectModelInput()` 生成：①同域形状快照块（已由历史/最近群消息/当前输入完整序列化的身份只给 `ref`+`duplicate_note`）；②跨渠道证据块（同上）；③新增一条“统一时间与权限”最小 system 说明 |
| 私下回顾 `generatePrivateReview`（Reflection/Concern/OpenWake/ActivityReview 共用） | 旧快照 JSON + thread 各一份来源/历史 | 快照块改为统一目录投影：thread 已完整携带的来源/历史/prior/concern/consulted/reminder 在快照中只给 `ref`；时间语义继续使用既有服务端 `time_context`（不重复插入时间块） |

代码：`cognitivecontext.h/cpp` 的 `projectModelInput()`/`CognitiveProjection`/`CognitiveProjectionStats`；
`LLMService::setCognitiveContextActualEnabled()`（环境变量 `MAKO_COGNITIVE_CONTEXT_ACTUAL=1`，
默认关闭）；前台接入点 `llmservice.cpp` prepareReply（约 1397 起），回顾接入点
`generatePrivateReview`（约 2194 起）；报告通过 `lastCognitiveShadowReport()` 读取
（`actual_mode=true`、`projection` 子对象）。

### 9.2 未接管的路径（保留旧逻辑及原因）

| 保留项 | 原因 |
|---|---|
| `CURRENT_UNDERSTANDING` catalog 组装 | 其证据编号 `ref:N`、`evidence` 字段与 `evidence_decision`/consult 契约强绑定；改为统一目录生成需要单独迁移编号契约，R12-3b 处理 |
| `MEMORY_DATA`（PromptAssembler） | 派生语义记录（profile/episodic/summary/claim/relationship），与原始事件不是同一身份；旧 shapes 与模型引用习惯稳定，先不动 |
| 短期历史 / `RECENT_GROUP_CONTEXT` | 它们是原始事实的“权威全文”位置；actual 投影正是以它们为 `alreadySerialized` 基准，保留全量 |
| 关注/活动/提醒/能力清单消息 | 内部语义记录（各自有独立身份），不是同一原始事件的重复；保留现有注入与写回契约 |
| 工具协议 `memory_recall`/`memory_read`、matter consult、`verifyActionProposal`/`VerificationBinding`/`ActionPolicy` | 服务端数据通路；本切片不改变其输入输出。actual 模式只改模型数据块的文本位置，不删除任何 evidence 引用 |
| Outbox/Reminder 发送与回执 | 独立执行链，本切片未触碰 |

### 9.3 资源与重复度实测（自动测试，非生产数据）

**(a) 前台 mock fixture**（3 轮同主题历史；`unifiedContextActualForeground`）：

| 指标 | 结果 |
|---|---|
| 旧组装 chars → 投影后 chars | 8145 → 7666（saved_chars=479） |
| 重复证据身份数 / 改写为引用的条目 | 6 / 10 |
| 去重后完整证据（本投影内） | 0（正文全部已在历史中，引用替代） |
| 引用解析 | 3 emitted / 3 resolved / 0 unresolved |
| 隐私过滤 / 泄漏 | 0 / 0 |
| 模型主请求数（baseline 一轮 + actual 一轮） | 各 1，无额外请求 |
| 预算 | snapshot_budget_bytes=4096，未触发裁剪 |

**(b) 回顾四来源**（`unifiedContextActualReviewKinds`：reflection/concern/open_wake/activity）：
每个来源 `actual_mode=true`、`refs_unresolved=0`、`privacy_leaks=0`、回顾请求数各 1；
快照投影出现“去重项只给 ref”说明，时间语义由既有 `time_context` 提供。

**(c) 真实模型集成**（隔离临时库，DeepSeek 真实 provider，不发渠道消息；
`realModelUnifiedContextIntegration`）：

| 指标 | 前台 | 回顾 |
|---|---:|---:|
| 实际 input chars / baseline chars | 6560 / 6763（saved 203） | 4478 / 4403（saved −75） |
| 实际 input bytes / baseline bytes | 12396 / 12163（saved −233） | 7530 / 7329（saved −201） |
| duplicate evidence / 改写为引用 | 2 / 4 | 2 / 2 |
| refs resolved | 1/1 | 2/2 |
| privacy filtered / leaks | 0 / 0 | 0 / 0 |
| prompt_tokens（provider usage） | 3403 | 1980 |
| 模型请求 | 真实请求各 1（前台/回顾），无 shadow 增量 | 同左 |
| 结果 | provider 接受投影后的输入并返回可解析 JSON；输出格式（tags）被既有输出守卫生效，属模型随机性，不计本切片回归 | 回顾完成，无网络错误 |

**关于 saved_bytes 为负的说明**：本切片新增的“统一时间与权限”说明（前台）与 JSON 引用项
（`dedup`/`duplicate_note`/`policy_view`）带来固定开销；在“历史很短、重复只有几条”的 fixture 里，
字符节省可能小于这些开销，因此按字节看未必下降。验收口径是**重复正文只在最原始位置序列化一次**
（出现次数/改写条目数/引用解析率），不是强行压缩总字节；真实长会话下节省应更明显，需后续运行观察。
不通过删除必要资料来换取缩减。

### 9.4 12 条验收条件 → 证据映射

| # | 条件 | 证据 |
|---|---|---|
| 1 | 前台与后台引用同一原始证据身份一致 | `snapshotBuilderMaterialsShareIdentity`、`cognitiveShadowSharesIdentityAcrossEntries`、actual 回顾来源轮次 dedup |
| 2 | 实际输入重复证据减少 | `unifiedContextActualForeground`：重复文本出现次数下降（baseline≥2 → actual 更少）、dup_omitted=10 |
| 3 | 去重后引用仍可解析 | 报告 `refs_unresolved=0`；测试逐条按 `lastEvidenceDirectory()`/统一身份回查 |
| 4 | 历史事实与当前状态不混淆 | 保留 CURRENT_UNDERSTANDING/回顾 thread 语义；时间说明明确资料时间；slice1 matter/version 测试 |
| 5 | 跨午夜时间表达正确 | `timeSemanticsAreServerOwned`（今天/昨天/明天由服务端 now 计算）；actual 时间块规则明确旧相对时间不是当前时间 |
| 6 | 私有资料不泄露 | `projectionGroupHasNoPrivateDomains`、`memoryDataClaimsReuseAccessPolicy`、actual 报告 privacy_leaks=0 |
| 7 | 检索失败不伪装 empty | `retrievalFailureIsNotEmptyHistory`（degraded/error/not_queried/empty 四态） |
| 8 | 记忆工具与 Matter consult 无回归 | 全量 ctest（tool/matter 用例）；actual 未改工具协议与 consult 数据通路（风险：actual+tool 运行时未单独覆盖，见 §9.6） |
| 9 | 主动联系/分享/核验无回归 | 全量 ctest（`tst_actionoutbox`/`tst_actionpolicy`/`tst_memoryagent` 行动链）；verifier/binding 服务端路径未改 |
| 10 | 用户委托提醒无回归 | 全量 ctest（reminder 链）；actual 仅把快照 commitments 重复项改为 ref，提醒消息仍完整 |
| 11 | 普通前台回复无回归 | actual 关闭时旧 messages 逐字节不变；actual 开启 mock/真实模型均能完成回复；全量 ctest |
| 12 | 不额外增加模型请求 | actual 前后 main 请求计数 1/1；回顾四来源各 1；`modelRequestCount()` 只统计真实 post |

### 9.5 本轮验证与构建

- 构建：`build/stage-b`（Ninja Debug，Qt 6.11.1 / MinGW 13.1）全量通过
  （日志 `build/stage-b/r12-3a2-build.log`）。
- 全量 `ctest`：**24/24 通过**，127.23s（`build/stage-b/r12-3a2-ctest.log`）。
- `tst_cognitivecontext 20 passed`（新增 4 个投影单测）；
  `tst_memoryagent 120 passed / 5 skipped`（5 个 opt-in：真实模型/真实库）；
  `tst_memorymanager 157 passed / 1 skipped`。
- 真实模型集成：`build/stage-b/r12-3a2-real-model.log`（隔离库，未发真实渠道消息）。
- 生产库/配置未修改（v33、`contact_daily_limit=0`、`snapshot_budget_bytes=4096`；
  SHA-256 与 mtime 与基线核对一致）；未启动生产应用。
- 顺带修复既有测试的时间边界脆弱性：`tst_actionoutbox::verifiedContactHonorsSwitchesAndGate`
  原先固定 00:00–23:59 安静窗口，在 23:59 分钟会失败；改为按当前时刻 ±2 分钟动态构造。

### 9.6 剩余风险与未验证项

1. **actual+tool 模式运行时未单独覆盖**：actual 投影发生在 tool 分支之前，工具协议/工具结果追加
   未改；但“actual 开启 + memory_recall 工具轮”没有专门集成用例，建议真实运行观察。
2. **actual 开启时的真实行动链（contact/share/verifier）未单独跑真实模型**：服务端核验与
   Outbox 路径未改；全量测试在 actual 关闭下通过，actual 只影响模型输入块。
3. **saved_bytes 在短上下文可能为负**（见 9.3）：本切片以“重复正文单次序列化”与引用解析为
   验收口径；真实长会话收益待运行观察。
4. **回顾快照块与 thread 的语义分工**：thread 保留全量、快照只给引用；若模型更依赖快照字段，
   需观察回答质量。
5. **统一时间块只在无 `time_context` 时补充**：前台新增、回顾沿用既有 time_context；
   两处文案不同但语义一致，后续可统一（不影响本切片验收）。
6. **生产默认关闭**：`MAKO_COGNITIVE_CONTEXT_ACTUAL` 未开启时行为与第一切片/旧逻辑一致；
   真正切生产默认需单独授权与运行观察。

### 9.7 可退役的旧机制（现在不删，等 R12 完成/B5 自然运行验收后集中处理）

| 旧机制 | 退役条件 |
|---|---|
| `cognitiveSnapshotMessage` 内“直接序列化快照 JSON”的旧路径 | actual 模式在生产观察期无质量回退，且 R12-3b 把 CURRENT_UNDERSTANDING/MEMORY_DATA 也接入统一目录后 |
| “跨渠道记忆资料”块的原样 JSON 序列化 | 同上；需确认工具/consult 仍能拿到等价来源 |
| `CognitiveContextBuilder` 的 shadow 对照分支 | actual 成为默认且覆盖指标稳定后，保留只读统计即可 |
| 旧 `m_run.evidence` 临时引用表 | 统一目录提供稳定引用解析且工具/action 全链路迁移后 |

**新增模块职责边界**：`cognitivecontext.*` 只负责“证据身份/语义/时间/覆盖/投影/对照”；
不持有数据库、不发请求、不写库、不做调度；LLMService 仍负责消息组装与模型调用。
