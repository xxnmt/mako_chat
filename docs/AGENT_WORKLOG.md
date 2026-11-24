# Agent 工作记录（Handoff）

> 更新：2026-11-22 23:45 · 基线提交 `c04ba78`（`feat(image): 接入QQ图片即时识别功能`）
> 读者：接手的 agent / 开发者。
> 配套方案：[B 阶段认知连续性：复核与实施方案](B_STAGE_CONTINUITY_PLAN.md)（§11 是本次落地记录）。
> 原则：**只写可核验的事实**。每条结论带 file:line 或命令；不要凭记忆改代码。

---

## 0. 怎么用这份记录

1. 先读 §1 判断该不该动、动哪一块。
2. 按 §4 代码地图定位，按 §5 看已落地范围。
3. 接手前跑 §6 的构建与测试基线；`tst_runtimelog` 在本宿主环境会失败（见 §6.2），不要把它当回归。
4. 继续开发从 §9 的 P1 清单第 1 项开始，一项一提交、一项一验收。
5. 协作纪律见 §2.3：**源码只有一个写者，测试文件独立，构建只有一个执行者**。

---

## 1. 当前状态

| 批次 | 内容 | 状态 | 证据 |
|---|---|---|---|
| 评审 | 只读复核评审结论、定位根因 | 完成 | 方案 §2 |
| **P0** | 关注解析器、前台写回、提示词校准、可观测性、队列死信 | **已落地并通过回归** | 方案 §11、`tests/tst_concern.cpp` |
| P1 | 到期回顾（主动联系）、v23 关注字段、触发器、next_chat 消费 | **未开始** | 方案 §5 P1、本文 §9 |
| P2 | 发起者/承诺方归因、主动消息进历史、视觉跨轮 | 未开始 | 方案 §5 P2 |
| P3 | 检索性能、场景 fixture、文档对齐 | 未开始 | 方案 §5 P3 |

**本轮真正改变的行为**（可直接演示）：

1. 主人回答了她此前关注的事（例如"很好吃哦……没有荷包蛋"）时，答案到达的那一轮就会把关注标成 `resolved`，之后不再重复追问。
2. 私下回顾不再要求"旧关注的来源轮次恰好被检索命中"：按关注文本与当前发言的相关性解析 `prior`。
3. 前台每轮会注入最多 2 条**与本轮话题相关**的关注（此前是"最近一条"，常离题），并带 `concern_updates` 写回契约。
4. 队列死信只对不可恢复错误计数，且 `--memory-queue-status` 可看积压。

**仍未改变**：主动联系（`now`）仍然是 0 次——因为"到期再进入"的入口（P1）还没做。**不要用提示词继续放松来"催"主动**，那只会产生无理由联系。

---

## 2. 本会话的协作结构

### 2.1 角色与产出

| Agent | 类型 | 职责 | 产出（被采纳的部分） |
|---|---|---|---|
| `lead`（root） | 主控 | 评审复核、方案、P0 实现、集成、构建与验收 | 方案文档、P0 全部源码改动、本记录 |
| `matter-audit` | 只读审计 | 关注/回顾链路 | 定位 prior 发现机制、`next_chat` 只写不兑现、24h 领取窗口、`reflection_change` 写点存在但不可达 |
| `foreground-audit` | 只读审计 | 前台注入/认识更新 | 回顾注入是 `role=user`、审计无"是否引用回顾"字段、`attempts` 语义、主动消息不进历史、视觉不跨轮 |
| `retrieval-audit` | 只读审计 | 统一检索 | 查询只由当前消息构造、`MemoryEvidence` 与关注无关联、全库扫描 + 每 chunk 一次向量 SQL |
| `autonomy-audit` | 只读审计 | 自主触发/投递 | 只有两条注意力入口、33/33 回顾不选 `now`、`proactive.schedule` 未接线、`canContactNow` 依赖写作开关 |
| `p0-tests` | 实现 | 新建关注回归测试 | `tests/tst_concern.cpp`、`tests/CMakeLists.txt` 注册 |
| `p0-review` | 只读对抗审查 | 审 P0 diff | 2 个高危（`reflection_change` 主键冲突、匹配过宽）+ 7 项，已在 P0 修复或记入 §8 |

### 2.2 关键时间线

1. 4 个只读审计并行 → 复核方案（§2 的三处修正 + 一处补强）。
2. 定接口（`memorymanager.h`）→ `p0-tests` 并行写测试，`p0-review` 并行对抗审查。
3. lead 实现 `privatereflection/matterstore/llmservice/appcontroller/coordinator/main`。
4. 构建 + 测试迭代：修复 3 个失败（旧测试契约、SQL 空串绑定、`reflection_change` 冲突）。
5. 全量构建通过、`ctest` 19/20（唯一失败为环境性）、`tst_concern` 10/10。

### 2.3 协作纪律（下次照做）

- 只读审计 agent 不许改文件、不许构建。
- 源码改动由**一个** agent 统一做，避免头文件/接口竞态；测试文件可以独立 agent 写。
- 构建目录只有一个执行者，避免 `mingw32-make` 并发写坏对象文件。
- 结论必须带 `file:line`、日志行号或 SQL 查询结果；"我记得/应该"不算证据。
- 测试不通过时**先判断是实现错还是契约变了**；契约变了就改断言并写明原因，**不要改测试输入去迁就实现**。

---

## 3. 实机基线数据（2026-11-22 22:17 采样）

采样对象：`build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/app_data`（正式库副本，只读）。

| 事实 | 数值/证据 |
|---|---|
| 私下回顾总数 | 33（11/21 8 条 + 11/22 25 条） |
| 联系决定分布 | `next_chat/retained` 27、`private/retained` 6、`now` **0** |
| 关注修订 | `reflection_change` 0 行；`private_reflection` 33 行全部 `consider`、version 全 1 |
| 认识库 | `matter_revision` 211 行 / 93 个 matter（与"茉子的关注"不是一回事） |
| 卡住的认识更新 | `cognitive_source.id=179`：`attempts=13`、`last_error=response_truncated` |
| 关键日志 | `[Reflection] reviewing 519` → `history=2 prior_turn=0` → `committed 519 consider next_chat` |
| 前台已看到旧关注 | 18:54 轮提示词注入 `reflection/514`（日志 1428 行）；审计 `id=424` `provided_evidence` 含 `reflection/514`，`cited_evidence=[]` |
| 投递能力证明 | 08:05/08:40 两条提醒 `dispatch true` → `receipt "protocol_accepted"`（无用户新消息） |

复现查询（对库副本）：

```sql
select state,count(*) from private_reflection group by 1;
select timing,outcome,count(*) from reflection_contact group by 1,2;
select count(*) from reflection_change;
select id,attempts,last_error,outcome,due from cognitive_source order by id desc limit 5;
select id,recorded,data from cognitive_audit where kind='answer_decision' order by id desc limit 8;
```

---

## 4. 代码地图（关注链路，P0 后）

| 职责 | 位置 | 说明 |
|---|---|---|
| 领取一条新来源回顾 | `src/memory/privatereflection.cpp` `claimPrivateReflection` | 最近 24h、`handed_off/quiet`、无活动念头；预算在 `thought_review_log` |
| 构造回顾上下文 | `privatereflection.cpp:125` `reflectionHistory` | 先 `matchedConcerns`（`prior_reason=match`），再 ranked 兜底（`=ranked`），否则 `none` |
| **关注解析器** | `privatereflection.cpp:183` `matchedConcerns` | note 的 2/3 字连续片段 × 整条当前输入；3 字命中或 ≥2 个 2 字命中；可按"早于某轮"过滤；最多 2 条 |
| 精确读取关注 | `privatereflection.cpp:264` `concernByHead` | 内部复核用（带来源渠道），不做 viewer 过滤，调用方必须再校验可见性 |
| **对话了结关注** | `privatereflection.cpp:282` `resolveConcern` | 事务；写 `concern_resolution`、复制 basis、`state='resolved'`；`evidence_ref` 必须是 `current_input` 或可读 `chat/N` |
| 回顾提交复核 | `privatereflection.cpp:367` `finishPrivateReflection` | prior 复核改为 `concernByHead` + 版本/可见性 |
| 前台注入 | `src/cognition/llmservice.cpp:709` | `matchedConcerns` 最多 2 条 + `concern_updates` 契约（`:728`） |
| 回顾请求/提示词 | `llmservice.cpp` `requestPrivateReview`/`generatePrivateReview` | 日志 `prior_found/prior_reason/prior_ref`（`:1192`）；内部打分字段不进模型 |
| 静默轮写回 | `llmservice.cpp:2680` `concernUpdatesReady` | 模型 `close/defer` 空句时也落库 |
| 协调器 | `src/app/cognitivecoordinator.cpp` | 60s `reviewIfDue`；`concernUpdatesReady` 落库与审计 |
| 正常回复写回 | `src/app/appcontroller.cpp:419` | 轮次保存后解析 `concern_updates`（兼容根节点），失败只审计不回滚回复 |
| 队列取用/退避/死信 | `src/memory/matterstore.cpp:173` `deferObservation`、`:211` `observationQueueStatus` | 死信只对不可恢复原因计数 |
| 队列查看 | `src/app/main.cpp` `--memory-queue-status` | 输出 pending、按 last_error 分组、最老 due |
| 表 | `private_reflection` / `reflection_basis` / `reflection_contact` / `reflection_change` / **`concern_resolution`** | 最后一张是 P0 新增（幂等补表） |

---

## 5. P0 已落地清单

### 5.1 源码（11 个文件，+479/-28）

| 文件 | 改动 |
|---|---|
| `src/memory/memorymanager.h` | `reflectionHistory` 增 terms；新增 `matchedConcerns/concernByHead/resolveConcern/observationQueueStatus` |
| `src/memory/privatereflection.cpp` | 解析器、精确读取、事务了结、prior 发现与复核（主体改动） |
| `src/memory/matterstore.cpp` | 死信上限按原因分类、队列状态 |
| `src/memory/memorymanager.cpp` | 幂等补表 `concern_resolution` + 索引 + 删除触发器 |
| `src/cognition/llmservice.h/.cpp` | 前台注入/契约、提示词、审计字段、静默轮写回、内部字段剥离 |
| `src/app/cognitivecoordinator.cpp` | `concernUpdatesReady` → 落库 + 审计 |
| `src/app/appcontroller.cpp` | 正常回复后的了结提交、审计、根节点兼容 |
| `src/app/main.cpp` | `--memory-queue-status` |
| `tests/tst_concern.cpp`（新） | 10 个用例 |
| `tests/CMakeLists.txt` | 注册 `tst_concern` |
| `tests/tst_memorymanager.cpp` | `reflectionReconsideration` 断言按新契约更新（不再断言"无 ranked 必无 prior"） |

### 5.2 新增表

```sql
CREATE TABLE IF NOT EXISTS concern_resolution(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  turn INTEGER NOT NULL,              -- 做出（了结）动作的那一轮
  prior_turn INTEGER NOT NULL UNIQUE, -- 被了结的关注头
  action TEXT NOT NULL,               -- 目前只有 'resolve'
  kind TEXT NOT NULL,                 -- answer/contradiction/cancelled/dropped
  recorded TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
```

为什么独立成表：`reflection_change.turn` 是主键且被"回顾链修订"占用；同一轮先被前台了结、再被回顾修订会永久 `defer`（对抗审查发现）。

### 5.3 生产库迁移证据（2026-11-22 23:43）

迁移前已备份：`app_data/memory/QianDaoMoZi_memory.db.bak_before_p0_<时间戳>`。

运行一次 `Windows_AI.exe --memory-queue-status` 后（该路径会走 `initDatabase`）：

- `PRAGMA user_version = 22`（未改版本号）；
- `concern_resolution` 表已创建，0 行；`forget_concern_resolution` 触发器已创建；
- 运行日志出现 `[MemoryManager]数据库初始化完成,结构版本为: 22` 与队列状态 JSON。

若需要回滚表：`DROP TABLE concern_resolution; DROP TRIGGER forget_concern_resolution;`（只影响 P0 新增，不碰旧数据）。

---

## 6. 验证

### 6.1 命令

```powershell
$env:PATH='D:\Qt\Tools\CMake_64\bin;D:\Qt\Tools\mingw1310_64\bin;D:\Qt\6.11.1\mingw_64\bin;'+$env:PATH
cd 'D:\QtProject\Windows_AI\build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug'
cmake --build . -j 8
ctest --output-on-failure
.\tests\tst_concern.exe
.\tests\Windows_AI.exe --memory-queue-status   # 看运行库积压
```

> 注意：`Windows_AI.exe` 是 WIN32/GUI 子系统，`qInfo` 不进控制台；`--memory-queue-status` 的 JSON 会写进**当次运行日志** `app_data/logs/logs_*.txt`（关键字 `"pending"` / `"by_outcome"`）。实测输出：`pending=11`、`oldest_due_epoch=0`（即在途项都还没到期）。

### 6.2 当前结果

- 全量构建：零编译错误（含 `Windows_AI` 与 20 个测试目标）。
- `ctest`：**19/20 通过**。唯一失败 `tst_runtimelog` 是宿主环境问题：该用例 `QProcess` 起子进程后读管道 stdout；把子进程 stdout 重定向到文件可看到 `ready`（已手工验证），且该测试只编译 `runtimelog.cpp`，与本轮 diff 无关。
- `tst_concern`：10/10；`tst_memorymanager`：66 passed + 1 skipped（`MAKO_REAL_DB` 未设置）；`tst_memoryagent`：通过。
- 519/514 行为：`reflectionHistory(519,1,{},terms)` → `prior.source_turn=514`、`prior_reason=match`；`resolveConcern` 后 `state='resolved'`、`concern_resolution` 恰 1 行、不再注入。

### 6.3 如果构建被文件权限挡住

本机 `build/` 曾因 Windows 文件权限（缺少当前用户的有效写权限）导致 `make` 无法创建临时文件。处理方式：用 DSH 的 `diagnose-windows-sandbox-acl` 技能脚本对失败路径做一次诊断+修复（会打印回滚命令），然后重复原构建。**不要**手工改权限或提权运行脚本。

---

## 7. 设计决策与备选方案

| 决策 | 选择 | 理由 / 被否决的替代 |
|---|---|---|
| 关注身份 | 沿用 `private_reflection.turn_id` | P0 最小改动；新建 `concern` 表要迁 33 条并改全部读取路径，留 P1 |
| 旧关注发现 | 文本连续片段匹配（3 字命中或 2×2 字） | 不加 embedding：现有检索每次全库扫描，再加一路会放大延迟；也不引入新依赖 |
| 匹配收紧 | 去掉"仅靠抽词常见词"的兜底 | 误匹配会导致**不可逆**误了结（无 reopen），宁可漏判 |
| 了结流水 | 新表 `concern_resolution` | 复用 `reflection_change` 会主键冲突并让回顾反复 `defer` |
| 结构版本 | 保持 22，幂等补表 | 改 23 要同步改 13 处既有版本断言；P1 正式 v23 再一次性迁移 |
| 提示词 | 不放宽成"鼓励主动"，改成"有新证据/到约定时间/活动有结果才可以 now" | 放松提示词只会增加无理由联系 |
| 队列死信 | 只对不可恢复原因计数 | 网络/存储属可恢复；5 小时断网不该永久打死条目 |

---

## 8. 已知限制与技术债（P0 范围内）

1. `resolveConcern` 不校验 ref 是否属于本轮注入的 provided 集合：模型幻觉写一个可见且开放的 `reflection/N` 仍可了结。P1 用 run 级 provided 集合过滤。
2. **没有 reopen 路径**：误了结不可逆。P1 至少提供管理员/审计重开。
3. 匹配只看 `note` 文本，没有显式 `topic/watch`；同义改写（"好吃"↔"味道如何"）会漏。
4. `reflectionHistory` 的候选是最近 48 行；长期未了结的旧关注可能永远排不进候选（starvation）。
5. `replyQuiet` 写回已接，但 `AppController` 不在任何测试目标里，端到端只有 DB/协调器覆盖。
6. `tst_runtimelog` 在当前宿主环境失败（见 §6.2），与代码无关。
7. 审计 `concern_update` 的 `source` 传 0、`event_id` 放 data；与 `cognitive_source.id` 语义已隔离。

---

## 9. P1 待办（按顺序，可直接开工）

| # | 任务 | 关键文件 | 验收 |
|---|---|---|---|
| 1 | **v23 迁移**：`private_reflection` 加 `root_turn/topic/watch/next_check_at/resolved_by/resolved_kind/matter/initiator/claimed_by`；回填 `root_turn=turn_id`、`next_check_at=updated_at+12h` | `memorymanager.cpp` 迁移区 | v22→v23 重开不丢数据；旧库 33 条关注可读；`PRAGMA user_version=23` |
| 2 | **到期回顾**：`claimDueConcern`/`finishConcernReview`/`deferConcernReview`；协调器优先级（活动→到期关注→新来源）；`concern_review` 提示词 | `privatereflection.cpp`、`privatereview.cpp`、`cognitivecoordinator.cpp`、`llmservice.cpp` | 无新消息时到期关注被领取，产出 `keep/revise/resolve/contact` 之一；48h 内必有处理；预算仍走 `thought_review_log` |
| 3 | **触发器 T2**：新输入后 `wakeConcernsByTerms`；`understandingChanged` 挂接；群聊"记录但不回复"消息挂接 | `privatereflection.cpp`、`cognitivecoordinator.cpp`、`raweventprocessor.cpp` | 新证据到达 60s 内关注 `next_check_at=now`；无命中不触发 |
| 4 | **触发器 T3**：`finishThoughtReview` 发 `activityStateChanged`，关联活动/作品后置 `next_check_at=now` | `memorymanager.cpp`、`cognitivecoordinator.cpp` | 活动完成后一次到期回顾能看到活动结果 |
| 5 | **主动联系返工**：`canContactNow` 与 `privateWriting` 解耦；`now` 必填理由/触发来源；`reflection_contact` 记录 attempts/last_outcome；投递日志 | `appcontroller.cpp`、`privatereflection.cpp`、`llmservice.cpp` | 一次 `now` 完整走到 `protocol_accepted`；无理由不投递；负向对照 24h 0 条 |
| 6 | **next_chat 消费**：前台注入匹配关注时带 `contact_timing/outcome`，允许多关注自然提起 | `llmservice.cpp` | 主动聊起"上次你说……我还没问"；不是每轮都说 |
| 7 | **验收**：场景 A（到期/新证据 → 一次有理由的 `now`）、场景 B（跨天/换渠道/重启保持已了结） | `tests/fixtures/`、`tst_concern`/`tst_memoryagent` | 见方案 §6.1/§6.2 |

**P1 开始前先做**：用 `--memory-queue-status` 记录运行库积压基线；确认 `reflection_contact` 里没有 `now`（继续从 0 开始）。

---

## 10. 接手检查清单

- [ ] `git status` 确认工作树，基线 `c04ba78`。
- [ ] 按 §6.1 构建并跑 `ctest`，接受 `tst_runtimelog` 环境失败。
- [ ] 读 §4 代码地图，重点 `matchedConcerns` / `resolveConcern` / 前台注入三处。
- [ ] 跑 `.\tests\Windows_AI.exe --memory-queue-status`，记录 pending/死信。
- [ ] 从 §9 第 1 项开始，一次只改一个关注点；改完跑 §6.1。
- [ ] 改测试断言时写明"契约变更原因"，不要改输入数据绕开失败。

---

## 附录 A · `tst_concern.cpp` 用例

| 用例 | 断言 |
|---|---|
| `concernMatchedWithoutRetrievalHit` | rankedTurns 为空时仍返回 `prior=旧关注`、`prior_reason=match`（核心回归） |
| `matchedConcernsScoresByQueryText` | 相关输入命中；无关输入（含其抽词）为空 |
| `resolveConcernFromConversation` | 了结成功；`state=resolved`；`concern_resolution` 1 行；不再注入 |
| `resolveRequiresEvidence` | 空证据/非法 kind 拒绝；状态不变 |
| `doubleResolveRejected` | 第二次了结失败，只有 1 条流水 |
| `finishPrivateReflectionMatchesPrior` | 回顾链按匹配 prior 提交 `resolve`，写 `reflection_change` |
| `priorReasonNoneWhenUnrelated` | 无关新消息无 prior；`prior_reason` 为 none/ranked；旧关注仍开放 |
| `commonWordsDoNotMatch` | "今天"这类常见词单独出现不命中；真同句仍命中 |

## 附录 B · 数据/日志定位

- 运行库：`build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/app_data/memory/QianDaoMoZi_memory.db`（**只读查询用副本**，避免锁）。
- 日志：`build/.../app_data/logs/logs_*.txt`；关注链路关键字：`[Reflection]`、`[Concern]`、`[Reminder]`、`[ReflectionContact]`。
- ACL 报告（若再次遇到权限问题）：`D:\QtProject\acl-reports-dsh\`。

---

## 2026-11-23 · B 阶段（B1–B5）开发记录

### 基线

- HEAD `38af0b7`（11/22 P0 版）。本轮按"最后一次日志"`logs_20261122_235021_131_08ba87506fcb.txt` 与目标文档重新推进；11/23 被放弃的改动未沿用。
- 生产库迁移前已备份（`QianDaoMoZi_memory.db.bak_before_v25_<时间戳>`），迁移后 `user_version=25`。

### Agent 分工

| Agent | 范围 | 交付 |
|---|---|---|
| `m1a-memory` | v23 迁移、文档频率匹配、关注持久化、到期回顾记忆层 | 完成（实现保留） |
| `m1b-cognitive` | `PrivateReview` 三级优先级、concern 提示词、协调器空闲条件 | 完成 |
| `m2-delivery` | v24、主动联系门控/重试、主动消息历史、分享回执 | 完成 + 5 用例 |
| `m3-attention` | v25 注意力事件、重启恢复、当前活动、B2 活动字段 | 完成 + 6 用例 |
| `lead` | 接口定义、集成、构建修复、验收、文档 | 全量 19/20 |

### 关键实现

- 迁移：v23 关注字段（topic/watch/root_turn/next_check_at/resolved_*/claimed_by）；v24 `reflection_contact` 尝试字段 + `delivery_ledger`；v25 `attention_event`/`attention_state`。
- 匹配：按开放关注语料的文档频率降权（称呼类高频片段不计数），替代硬编码词表。
- 到期回顾：活动 → 到期关注 → 新来源；`concern_action` = keep/revise/resolve/drop/contact。
- 主动联系：与写作开关解耦；`rejected` 最多补一次，`unknown/handed_off/protocol_accepted` 不重发；主动消息写 `chat_history(turn_kind='proactive')` + `raw_events(direction='out')`。
- 注意力：启动扫描 `received` 轮入队 recovery、`due_wake`/`message`/`activity_result` 事件、当前活动与切换原因；B2 活动来源/产物/删除防护。

### 验证

- 构建零错误；`ctest` 19/20（唯一失败 `tst_runtimelog`，宿主进程管道环境问题）。
- `tst_memorymanager` 77 passed / 1 skipped；`tst_concern`、`tst_memoryagent` 通过。
- 生产库 v25 迁移验证：`attention_event/attention_state/delivery_ledger/concern_resolution` 存在、`private_reflection` 新列齐全、36 条开放关注回填。

### 事故与教训（必读）

- 用 PowerShell `Get-Content/Set-Content` 改写源码，导致 `tests/tst_concern.cpp` 编码损坏且不可逆；已 `git checkout` 恢复，并把 M1/M3 的验收用例改写入 `tst_memorymanager.cpp`。**后续只允许用编辑工具改文本文件，禁止用 PowerShell 重写源码。**
- 不面向结果：删除"欧尼酱"词表补丁，改为 DF 机制；测试用多关注语料锁机制并加正向对照，而不是改输入迁就断言。

### 仍未完成

B5 的 7 天实机连续运行、真实 QQ 主动联系到达、多活动并行选择、元数据优先阅读门控。

---

## 2026-11-23 · 改进批次（M5）

针对用户提出的"重要事件记忆、能力自感知、主动可选项、设置项、日志命名"五个问题：

| Agent | 范围 | 交付 |
|---|---|---|
| `r1-memory-config` | `MemoryEvidence` importance 进入检索与重排；proactive 新增三项配置；联系日限额、关注默认间隔/保留期执行端约束；设置页字段 | +4 用例（召回排序、日限额、默认间隔、保留期）+ config/settings 用例 |
| `r2-capability-log` | `LLMService::capabilityManifest` 与前台/回顾注入；日志会话头（version/pid/qt/build/config_hash）；CMake 版本 | +3 用例（清单配置联动、前台注入、日志头） |
| `lead` | 集成、构建修复（QByteArray::contains 类型）、全量回归、文档 | ctest 20/20 |

关键决策与边界：
- importance 只加在启发式重排（`+0.75*importance`），无 importance 时原分数不变；DashScope 重排不变。
- 能力清单由配置与服务端状态生成，只列真正可用项；不可用项给 `enabled:false` + reason。
- 日志不改文件名（毫秒时间戳已够），改为新增会话头与配置指纹；保留期/轮转未做。
- 能力清单尚未包含 `current_activity`（当前活动只持久化未注入），列为下一步。

---

## 2026-11-23 · 可靠性补丁（R3/R4）

实机日志暴露：QQ 关闭时普通回复 3 句被丢弃且无记录；到期修订覆盖旧想法；启动残留 `pending` 回顾；能力清单缺当前活动；空闲日志噪声。

| Agent | 范围 | 交付 |
|---|---|---|
| `r3-reply-delivery` | `delivery_ledger(kind='reply')` 逐句入账、`sendResult` 落状态、`rejected` 最多补一次、`unknown` 不重发；5s 定时器与重连触发补发 | +2 memory 用例 +1 QQ 断连用例（1×unknown + 2×rejected） |
| `lead` | v26 `concern_revision` 流水；`recoverStaleReflections` 启动恢复；能力清单 `current_activity`；日志降噪 | +3 用例；迁移 v26 |

关键决策：
- 普通回复的账本 ref 固定为 `reply:<turn>:<index>`，与主动消息 `contact/reminder/share` 共用 `delivery_ledger`。
- `unknown` 永不重发（协议可能已送达）；`rejected` 最多补一次；补发不受安静时段限制（补的是已决定要说的话）。
- 到期回顾原地更新，故单独用 `concern_revision` 保存决策前后快照；来源回顾仍用 `reflection_change` 修订链。
- 启动恢复只处理"租约已过期"的 `pending`：到期关注回 `consider`、来源回顾回 `deferred`，避免误抢进行中的回顾。

验证：构建零错误；`tst_memorymanager` 85 passed/1 skipped、`tst_memoryagent` 73、`tst_qqdelivery` 6。`tst_runtimelog` 为宿主间歇性环境失败（3 次重试 2 败 1 过，失败点是既有子进程管道用例）。生产库已备份并迁移 v26。
