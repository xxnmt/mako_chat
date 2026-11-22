# B 阶段认知连续性：复核与实施方案

> 版本 0.1 · 2026-11-22 · 基于只读评审 + 运行库/日志核对
> 目标读者：项目维护者。本文只回答两件事：接下来怎么改、改成什么样算通过。
> 前置：`构想.txt`（愿景）、`STATUS.md`、`ROADMAP.md`、`ARCHITECTURE.md`。
> 本文档未修改任何现有代码；所有改动均为"待实施"，行号以当前工作树为准。
> **2026-11-22 23:40 更新**：P0 已落地并通过回归（实机证据见文末 §11）。本文其余部分仍是 P1–P3 方案；其中 §4.2 的 v23 字段清单在 P0 中尚未实施——P0 只新增了 `concern_resolution` 补表，没有加 `next_check_at/topic/watch` 等列。

---

## 0. 一页结论

1. **评审结论总体成立**，但有三处需要修正、一处需要补强（见 §2）。
2. **真正的缺口不是"没有关注对象"，而是没有一条统一的"关注解析器"。** 系统已经有三套自我状态存储（主人认识、私下想法、活动作品），也有 `revise/resolve`、`reflection_contact`、`reflection_basis` 这些机制；缺的是让同一条用户消息在前台与后台**解析出同一份"我关心什么"**，并让交流结果写回。
3. 11/22 那个具体失败（你说"很好吃、没有荷包蛋"，她第二天还在问吃饭）不是单点 bug。实机证据显示：**前台那一轮已经把 `reflection/514` 注入了提示词，模型也读到了答案内容，但前台没有写回权限；后台回顾那一轮只看检索命中，连 514 都没拿到。** 两条路径各算一遍，才是根因。
4. **主动对话 0 次不是投递链路坏了。** 投递链路完整、QQ 有协议回执、提醒同一条链路在 08:05/08:40 成功过。0 次是因为**没有任何场景会让她合法地选择"现在"**：配置里的私聊 QQ 目标只负责"发给谁"，不负责"何时发起"；唯一的主动私聊决策入口是"刚收到一条消息后的空闲回顾"，而那里她每轮都被提示"无需联系任何人"。
5. 建议按 **P0→P3 四批**推进，不推倒重来。P0 只做接线与可观测性，用 519/514 的真实失败样本回归；P1 补"关注"的数据模型与到期再进入；P2 补归因与可追溯；P3 收性能与文档。
6. **验收主体是普通关注，不是写作、不是定时提醒。** 写作只能作为"有进展"的一种来源，不能作为主动性的主要证据（否则会高估普遍的主体连续性，评审里这一点是对的）。

---

## 1. 现状：三条状态链 + 两条注意力入口

### 1.1 三套存储，互不引用

| 链 | 表/列 | 内容 | 实机规模 | 谁写 | 谁读 |
|---|---|---|---|---|---|
| **主人认识链** | `cognitive_source` → `matter_revision` → `observation_history` | 主人说过的事、事项版本、生命周期 | 211 条 revision / 93 个 matter / 188 条来源 | 后台观察流水线（模型提案 + 服务端校验） | 前台 `currentUnderstanding`、群聊 `internalMatters` |
| **私下想法链** | `private_reflection` → `reflection_basis` → `reflection_contact` → `reflection_change` | 她对某轮经历的自己的念头、联系决定、依据 | 33 / 2 / 33 / **0** | 私下回顾（LLMService） | 前台 `recentPrivateReflection`（每轮最多 1 条） |
| **活动作品链** | `chat_history.thought / thought_result / thought_version / review_after` | 写作兴趣、草稿、进度、下一步 | 1 个活动（`thought/492`） | 回顾 + 前台 `activity_action` | 回顾 `nextThought`、前台注入 |

三链之间**没有外键**。"关注"没有自己的身份：它只是"某一轮的 private_reflection 行"，身份是 `source_turn`。

### 1.2 两条注意力入口

```
                     ┌───────────────── 新用户消息 ─────────────────┐
 [前台] submit ──────► prepareReply ─► 检索 top12 ─► 注入 1 条想法 ─► 模型 ─► 保存轮次
                     │                     │                          (无写回)
                     │                     └─ 不参与"关注"解析
 [后台] 60s idle ────► 活动到期? ─► 私下回顾(新来源) ─► 检索 top6 ─► prior 必须命中 ─► 模型 ─► 写回
                     │                                                             │
                     └─ 没有"关注到期"入口                                          └─ next_chat = 永不投递
 [提醒] 5s 扫描 ─────► 发送已存文本（不经过模型）
```

- `CognitiveCoordinator::reviewIfDue()` 每 60s 检查一次；条件 `privateWriting && !busy && idle>=idleSeconds`（当前 300s）。
- `PrivateReview::wake()` 先试到期活动，没有才领取新来源回顾。**没有第三种"关注到期"入口。**
- 提醒定时器只发固定文本，不触发模型。

### 1.3 "关注"的四种载体，各自缺什么

| 载体 | 有 | 缺 |
|---|---|---|
| `private_reflection.note` | 来源轮次、状态、版本、依据 | 稳定身份、主题/待验证点、到期时间、了结状态 |
| `reflection_contact` | timing（private/next_chat/now）、理由、结果 | `next_chat` 没有消费者；没有到期；没有投递尝试次数 |
| `matter_revision` | 生命周期、确定性、完成条件 | 没有"谁发起/谁承诺"；生命周期不参与任何查询 |
| `thought_result` | 进度、下一步、状态 | 完成不产生任何事件 |

---

## 2. 评审结论复核

### 2.1 成立的结论（已用运行库/日志确认）

| 评审结论 | 复核证据 |
|---|---|
| 22:15 回顾触发了统一检索，找回 11/19、11/12 两条原话 | `reflection_basis` = `(519,464)`、`(519,359)`；464 = 11/19「很好吃哦，要是茉子也能吃到就好了」，359 = 11/12「有好好吃饭…作息不健康」 |
| `history=2 prior_turn=0`，旧关注没带上 | 日志 `logs_20261122_181400…:7551` 逐字一致；`cognitive_audit` 中无该轮的 concern 记录 |
| reflection/514 记的是"还没听他说好不好吃，下次想问" | `private_reflection.turn_id=514`，note 含「不知道拌开之后香不香…是不是有荷包蛋呀」 |
| 她又留下 next_chat 问吃饭/作息 | reflection/519 note：「下次聊天时想问问欧尼酱今天有没有好好吃饭，顺便提醒他别又熬到凌晨」 |
| 正式库的关注修订记录为 0 | `select count(*) from reflection_change` = 0 |
| 25 次回顾：19 下次聊 / 6 留给自己 / 0 现在联系 | 当日 `thought_review_log` 25 条；`reflection_contact` 27×next_chat + 6×private，0×now |
| 她记得你说过什么，却不稳定记得自己关心什么 | 514 仍是 `consider`，从未被任何后续轮修订或了结 |
| 留待下次缺少"后来谈过了"的反馈 | 前台注入是只读的，保存轮次没有关注写回路径 |
| 主动消息不进历史，换渠道/换轮就认不出 | 提醒/联系/分享都只调 `sendUtterance`，不写 `chat_history`/`raw_events` |
| 第 179 条认识更新尝试 13 次仍失败 | `cognitive_source.id=179`：`attempts=13`，`last_error=response_truncated`，`due=22:18` |

### 2.2 需要修正的三点

**修正 1：96 是配置，不是代码默认；而且它实际不构成约束。**
`identity.json:19` 写 `daily_limit=96`（文件 22:14 被改过）；代码默认是 3（`configmanager.h:40`、`configmanager.cpp:909`、`settingspages.cpp:603`、`CONFIG_REFERENCE.md:326` 都是 3）。同时 `cooldown_seconds=900`：24h/900s = 96，所以 **真正卡住频率的是冷却，不是日上限**。把 96 当作"系统设计上限"会误判容量。

**修正 2："关注修订记录为 0"要区分两个概念。**
- `reflection_change` = 0 是对的，指"私下想法被 revise/resolve"。
- `matter_revision` = 211 行、93 个事项、46 个事项有多次修订——这是"主人认识链"，不是"茉子的关注"，不能用 211 反驳 0，也不能用 0 说整条链路都没写入。
- v22（`reflectionHistory`/`prior`/`reflection_change`）是今天 18:12 才合入的（`git log -S`）；之后只运行过 1 次回顾（519，22:15）。所以"机制从不工作"证据不足。**真正的缺陷是 prior 的发现方式**，这一点被 519 实机数据证明。

**修正 3：`attempts=13` 不等于"13 次截断"，也不是"重试 13 次截断"。**
`attempts` 是"失败后重新排期"的累计次数；`last_error` 只保留最近一次。截断在"恢复模式"下才计拒绝，同一来源最多 3 次截断就会进入终态 `review_required`。13 次说明前面还有网络错误、非法 JSON、证据变化等其他失败类型。更严重的是：**非拒绝类失败没有次数上限、没有死信、`review_required` 不进日志**，所以"积压是否消除"在运行实例上不可观测。

### 2.3 需要补强的一点（比原评审更尖锐）

原评审说"检索到相似原话也可能绕过那个疑问"。这在 22:15 的后台回顾里成立。但**18:54 的前台那一轮并没有绕过**：

| 事实 | 证据 |
|---|---|
| 18:54 那轮（turn 519）前台检索的 reflection 命中列表包含 514 的来源轮 | 日志 `:1387` `reflection= QList("chunk/11598","chunk/9879","chunk/7534","chunk/7702","chunk/7682","chunk/7400")` |
| 该轮提示词里确实注入了 `reflection/514` | 日志 `:1428`（前缀「茉子私下回顾时留下的想法…」+ `"ref":"reflection/514"`） |
| 审计记录 `provided_evidence` 含 `reflection/514`，但 `cited_evidence` 为空 | `cognitive_audit.id=424`（记录时间 10:55:07 UTC = 18:55 本地）`reflection_provided=['reflection/514']`、`reflection_cited=[]` |
| 模型其实读到了答案内容 | 同一条审计 `basis`：「用户本轮自述吃完了、味道香香的油油的、**没有荷包蛋**，并希望和茉子一起吃」 |
| 但没有任何路径把 514 标成已答 | 保存轮次只写 `chat_history`，不写 `private_reflection`/`reflection_change` |

**所以：不是信息没到，是到了之后没有"写入权限"。** 前台和后台各有一套"我关心什么"的查询，两者结果不一致；而"了结关注"这个动作只存在于后台回顾里，且要求"旧关注的来源轮次恰好进入本轮检索 top-6"。

这决定了方案的第一原则：**做一个共用的关注解析器，而不是继续在两条链上各打补丁。**

### 2.4 追加排查：主动对话已开启，为什么从未发起、也很少提及回顾

**配置确实是对的（"私聊 QQ"在）**：`identity.json` `proactive.targets=[{channel:"qq_private",session_id:"2014366825"}]`，`enabled=true`、`private_writing=true`、安静时段 `23:59–00:00`。这些配置被四处消费：提醒投递（`appcontroller.cpp:216`）、联系投递（`:234`）、草稿分享（`:273`）、`canContactNow` 闸门（`:429`）；回顾提示词也据此追加联系选项（`llmservice.cpp:1202`）。**但它们只决定"能不能发、发给谁"，不决定"何时由谁发起"。**

**投递能力已被实机证明**：11/22 08:05、08:40 两条提醒在**没有用户新消息**的情况下发出，日志为 `[Reminder] dispatch 1 true` → `receipt "reminder:1" "protocol_accepted"`（`logs_20261122_015938…:1538-1539,1551-1552`）。说明"茉子向 QQ 私聊主动发一条消息"这条底层通路是通的。

**需要精确的一点**：`contact`（"现在联系"）分支本身**从未实机执行**——全量日志 0 条 `[ReflectionContact]`，DB 无 `proposed/dispatching/handed_off`。所以说"投递链路完整"指的是：代码齐备 + DB/协调器单测覆盖（`tests/tst_memoryagent.cpp:631-635`）+ 相邻提醒链路实机成功；不是这条分支被实机验证过。

**容易误读的地方**：`configmanager.cpp:905` 仍写着"主动对话配置（Phase 0 只解析不调度）"，`proactive.schedule` 也**确实只解析、无消费者**（`:920-928`）。这句话在今天已部分过时（提醒/回顾/分享已接线），但"自主发起"仍未被调度——配置里没有任何字段能让程序"到点自己找她聊"。

| # | 原因 | 证据 | 归入 |
|---|---|---|---|
| A0 | **配置只解决"发给谁"，没有"何时发起"的调度器**；`proactive.schedule` 只解析不消费 | `configmanager.cpp:905` 注释"Phase 0 只解析不调度"、`:920-928` 无消费者；主动输出只有"回顾决策"和"用户委托的提醒"两种来源 | P1 |
| A1 | **主动私聊的唯一入口是"回顾时模型自己选 now"**，而这类回顾总是由"刚收到一条消息"触发，此刻打断在局部看永远不划算 | `privatereview.cpp:57` 只在 `contact.timing=="now"` 时发信号；33/33 次回顾选的都是 next_chat/private | P1 |
| A2 | 提示词有三处直接抑制：「**无需创作或联系任何人**」「**联想到旧事本身不构成联系的理由**」「**无需为了主动而发送**」 | `llmservice.cpp:1176-1180`、`:1195`、`:1211` | P1 |
| A3 | **不存在"旧关注 + 时间过去 + 条件变化"的再进入场景**，而那才是主动联系唯一合理的时机 | 无 `claimDueConcern`；`private_reflection` 只在新消息到达时被领取；24h 窗口外永久失去回顾资格 | P1 |
| A4 | `next_chat` 语义是"下次主人开口时作为材料"，**系统永远不会主动兑现** | `claimReflectionContact` 要求 `outcome='proposed'`，只有 `now` 会写 `proposed`；`next_chat/private` 都是 `retained`，无消费者 | P0/P1 |
| A5 | 前台注入的是"最近一条"而不是"相关的"，且只注入 1 条；离题注入让模型学会忽略这条通道 | `recentPrivateReflection(meta, rankedTurns)` 默认 `requireRanked=false`，无命中时按 `updated_at DESC` 取最新；函数在 `while` 内 `return` 只给 1 条 | P0 |
| A6 | 提示词明确要求"不主动根据旧记忆追问，也不暗示私人来源" | 前台 `CURRENT_UNDERSTANDING` 的 `avoid_unprompted_followup` 说明（`llmservice.cpp:740-741`） | P0 |
| A7 | 回顾作为证据 `speaker_role=assistant`，用 `reported_fact` 引用会被判 `scope_does_not_support_fact`；提示词又没告诉模型回顾编号可引用；审计没有"是否引用回顾"字段 | `llmservice.cpp:709-710`、`evidenceaudit.h:31-44`、`:599`/`:604` 的编号示例只有 `ref:N`/`chat/339`/`current_input` | P0 |
| A8 | "现在联系"被间接绑定到"写作开关" | `canContactNow` → `idleForWriting()` 要求 `privateWriting=true` | P1 |
| A9 | 主动消息/分享不进历史，下一次她无法确认"这句话是我先说的" | 只调 `sendUtterance`；`turnKind=proactive` 在生产无写入点；分享没有 outcome | P2 |
| A10 | 没有度量。"没联系"不会产生任何日志/计数，0 和"根本没到判断"无法区分 | 无 `[ReflectionContact]` 日志行；审计无 `concern_*` 字段 | P0 |

> 结论：**不要用"鼓励主动"的提示词去修。** 提示词放松只会让"无理由联系"变多。正确的修法是先造出"有理由"：到期关注、新证据、活动完成三类触发；再让模型在有理由时判断，并把"为什么是现在"作为必填理由落库。

---

## 3. 目标校准

### 3.1 一个核心目标

**让一件普通关注有从产生到结束的完整过程。** 不是"提高回顾质量"，不是"增加主动消息"，也不是"扩写作"。

### 3.2 三个能力（对应用户建议）

| 编号 | 能力 | 落地位置 |
|---|---|---|
| **C1** | 同时找"相关经历"和"茉子尚在意的事"；当前消息可直接匹配旧关注、兴趣或约定，再回原始记录核实 | §4.4 双查询 + P0-1 |
| **C2** | 交流结果能改变关注：问过、答了、取消、改变主意都能更新或结束；保留发起者与来源 | §4.7 前台写回 + §4.2 模型 + P2 归因 |
| **C3** | 有理由的关注重新进入注意力：新证据、活动完成、自身认为有意义的时机；允许联系、延后、继续私藏、放下 | §4.6 三类触发 + P1 到期回顾 |

### 3.3 两个对称验收场景

| 场景 | 检验什么 | 通过的最低形态 |
|---|---|---|
| **A 主动**：一件她关心的事后来有了值得分享的进展，在没有新提问时她自己想起，并自主判断是否联系 | 她能否主动 | 到期/新证据触发一次有理由的 `now` 投递，取得 `protocol_accepted`；无理由时不联系 |
| **B 收尾**：一件事你已经给过答案，她隔天、换渠道、重启后仍记得这份关心已有下落 | 她能否理解你的生活确实在向前走 | 答案到达的那一轮完成 resolve；此后不再重复追问；被问起时能按证据回答 |

对称性来自：A 是"关注变重"（推进），B 是"关注变轻"（了结）。两个都必须用**普通私聊**验收；写作只在 A 中作为"进展来源之一"，不能是唯一来源。

### 3.4 非目标

- 不追求"主动消息数量"。主动消息数只作观察量，不作验收指标。
- 不在本阶段做人格/情绪建模、外部世界（浏览、绘画）、多角色。
- 不换检索后端、不引入新框架、不重写协调器。
- 不把"模型多引用回顾"当成功——那只会改善审计数字。

### 3.5 成功指标（观察量，非 KPI）

| 指标 | 目标 | 说明 |
|---|---|---|
| 关注了结延迟 | 答案到达 ≤1 轮内 resolve | 主指标，场景 B |
| 已了结话题重复追问率 | 0% | 7 天窗口，人工判读 |
| `now` 带理由且理由可核对 | 100% | 每条 `now` 必须有 reason + 触发来源 |
| 无理由 `now` | 0 | 安静日负向对照 |
| 未处理到期关注 >48h | 0 | 到期回顾必须处理或明确延后 |
| 关注开放时长 P50/P95 | 观察值 | 判断是否变成"永不结束的挂念" |

---

## 4. 目标模型

### 4.1 设计原则

1. **单一解析器**：前台、后台回顾、到期回顾共用同一个"关注与当前输入的相关性"查询（`ConcernResolver`）。
2. **关注有稳定身份**：`root_turn` 标识一个关注；修订是链（`reflection_change`），收尾是状态（`resolved/dropped`）。
3. **三类事件驱动**：新证据、时间到期、活动/作品状态变化。没有理由不进入注意力。
4. **模型判断，服务端校验**：模型只输出动作与理由；状态迁移、可见性、版本、事务由服务端做。
5. **预算统一**：继续用 `thought_review_log` 记账（回顾额度承担预算控制），不新增第二套预算。
6. **可观测优先**：每个"没发生"的原因也要能落地（日志/审计），否则无法区分"模型判断错"与"资料没处理到"。

### 4.2 数据模型 v23（全部为新增列/新索引，不重建现有表）

```sql
-- 关注成为一等对象：在 private_reflection 上扩展，保留原"每轮快照"语义
ALTER TABLE private_reflection ADD COLUMN root_turn    INTEGER;            -- 关注身份（首次产生它的轮次）
ALTER TABLE private_reflection ADD COLUMN topic        TEXT;               -- ≤60，关注对象短标签，用于匹配与展示
ALTER TABLE private_reflection ADD COLUMN watch        TEXT;               -- ≤200，什么证据能推进/了结它
ALTER TABLE private_reflection ADD COLUMN next_check_at INTEGER NOT NULL DEFAULT 0; -- 到期再考虑（unixepoch）
ALTER TABLE private_reflection ADD COLUMN resolved_by  INTEGER;            -- 了结它的 chat_history.id
ALTER TABLE private_reflection ADD COLUMN resolved_kind TEXT;              -- answer|contradiction|cancelled|dropped|merged
ALTER TABLE private_reflection ADD COLUMN matter       TEXT;               -- 可选：关联 matter_revision.matter
ALTER TABLE private_reflection ADD COLUMN initiator    TEXT NOT NULL DEFAULT 'mako';
ALTER TABLE private_reflection ADD COLUMN claimed_by   TEXT;               -- source_review|due_review（区分租约来源）
CREATE INDEX IF NOT EXISTS idx_reflection_open ON private_reflection(state,next_check_at);
CREATE INDEX IF NOT EXISTS idx_reflection_root ON private_reflection(root_turn);

-- 联系/投递可观察性
ALTER TABLE reflection_contact ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0;
ALTER TABLE reflection_contact ADD COLUMN last_outcome TEXT;

-- reflection_change 当前 0 行，可直接重建，去掉 PK(turn) 的限制
--   （原 PK(turn) 会导致"同一轮既了结旧关注、又被回顾修订"时插入失败）
DROP TABLE reflection_change;
CREATE TABLE reflection_change(
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  turn       INTEGER NOT NULL,             -- 做出修订/了结的那一轮
  prior_turn INTEGER NOT NULL UNIQUE,      -- 被修订/了结的关注头
  action     TEXT NOT NULL,                -- revise|resolve
  kind       TEXT,                         -- answer|contradiction|cancelled|dropped|merged
  recorded   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

**state 取值扩展**：`pending`（租约中）/`deferred`/`rest`/`consider`（开放）/**`resolved`**/**`dropped`**。

**迁移回填**：
- `root_turn = turn_id`（现有 33 行没有链，全部是根）。
- `next_check_at = updated_at + 12h`（给现有开放关注一次再进入机会；是否回填作为开放问题 §10-7）。
- `topic/watch` 留空，等下一次回顾补齐；不阻塞读取（匹配时对空字段降级为 note 匹配）。

### 4.3 关注生命周期

```
            领取(新来源/到期)              模型 consider
  [consider] ───────────────► [pending] ───────────────► [consider]
   (开放)        version+1       (租约 30min)     ┌────────┴────────┐
      │                                          │                 │
      │ 前台证据明确回答/否定/取消                  │ rest            │ resolve/drop
      ▼                                          ▼                 ▼
  [resolved] ◄──────────────────────────────────────────────  [resolved]/[dropped]
  resolved_by / resolved_kind / reflection_change              （终态，保留审计）
      │
      │ 新证据命中 / 时间到期 / 活动完成
      └──────────────► next_check_at = now ──► 到期回顾
```

- 开放关注集合 = `state='consider' AND NOT EXISTS(change WHERE prior_turn=turn_id)`。
- 已了结的**不再进入前台注入、不再进入到期回顾、不再被重复问**；但历史仍可回忆（`reflection_change` + basis 保留）。
- 被修订（revise）时：旧头 `state` 不变（仍 consider），新行成为头，`root_turn` 继承旧头的 `root_turn`，`reflection_change` 记一行。

### 4.4 关注解析器（C1 的核心）

新增 `MemoryManager` 接口（名称可调）：

```cpp
// 1) 与当前输入相关的开放关注：前台与后台共用。
//    不要求检索命中；用 terms 在 topic/watch/note 上打分，再叠加近因与同 matter。
QJsonArray matchedConcerns(const RequestMeta &viewer,
                           const QStringList &terms,
                           qint64 excludeTurn,
                           int limit = 2) const;

// 2) 精确读取（用于提交前复核与前台 ref 解析）
QJsonObject concernByHead(qint64 headTurn) const;

// 3) 前台写回
bool resolveConcern(qint64 headTurn, int version, qint64 byTurn,
                    const QString &kind, const QString &evidenceRef);
bool keepConcern(qint64 headTurn, int version, qint64 nextCheckAt);

// 4) 到期回顾
QJsonObject claimDueConcern(int dailyLimit, int cooldownSeconds);
bool finishConcernReview(qint64 headTurn, int version, const QJsonObject &proposal);
bool deferConcernReview(qint64 headTurn, int version);

// 5) 新证据唤醒（廉价的 SQL + 词面匹配，不调用模型）
int wakeConcernsByTerms(const RequestMeta &viewer, const QStringList &terms,
                        const QString &wakeReason);
```

**匹配算法（确定性、可测试、不引入 embedding）**：

1. 候选集：可见的开放关注，最近 14 天，`LIMIT 24`（SQL 一次取回）。
2. 打分：`score = Σ(term 命中 topic×3 + watch×2 + note×1) / 词数`，加上同 `matter` +2、`next_check_at<=now` +1、最近 3 天 +1。
3. 阈值：`score >= 0.6` 才进入候选；最多返回 2 条，按分排序。
4. 成本：O(24) 行 × 少量词；**不新增检索轮次、不新增模型调用**。这是为了不放大现有"每次检索全库扫描"的开销。
5. 前台注入与后台 prior 使用**同一个函数**，只是调用参数不同（前台 `excludeTurn=本轮回合`，后台 `excludeTurn=来源轮次`）。

**关键改动**：`reflectionHistory()` 不再用 `recentPrivateReflection(viewer, ids, true)` 从"检索命中的轮次"里找 prior，改为：
```
prior = matchedConcerns(viewer, terms, turn).first()
       ?? 旧的 rankedTurns 命中（兜底）
```
`finishPrivateReflection` 的提交复核同步改为 `concernByHead(previous)` 对比（不再依赖 `requireRanked=true`）。

> 这一条预期能修复 11/22 的失败：519 的 terms（好吃/香/荷包蛋/拌饭）与 514 的 note（拌开香不香/荷包蛋）词面重叠明显，514 应当作为 prior 进入回顾，模型可以选择 `resolve`。**阈值与打分必须在 P0 用这条真实数据验证**，不能只靠设计推断。

### 4.5 后台回顾的输出契约扩展

现有 `choice ∈ {rest, consider}`、`contact{timing,reason,message}`、`prior_action ∈ {revise,resolve}`、`consult{terms}`。新增：

```json
{
  "forget": {"topic": "烤肉拌饭", "watch": "他说好不好吃、有没有荷包蛋",
             "next_check_after_minutes": 240},
  "priors": [{"ref": "concern/514", "action": "resolve", "kind": "answer"}]
}
```
- `forget`（保留旧字段名避免歧义，实际建议改名 `concern`）：创建/更新关注的主题、待验证点、下一次再考虑的时间。
- `priors`：从单条 `prior` 扩为最多 2 条；每条独立 `action`。服务端校验 ref 必须是本轮提供的、仍开放、版本一致。
- `next_check_after_minutes`：0..10080；`0` 表示"只在新证据时再看"。缺省 720（12h）。
- 对 `now` 的判定顺序写进提示词：**先看有没有新证据/是否到约定时间/是否有活动结果，有才允许 now；只是好奇就 next_chat/private。**

### 4.6 三类触发器（C3 的核心）

| 触发器 | 事件源 | 动作 | 成本 |
|---|---|---|---|
| **T1 时间到期** | `next_check_at <= now` | 协调器 60s tick 中，在活动之后、新来源回顾之前领取一个到期关注 | 1 次模型调用（计入回顾预算） |
| **T2 新证据** | ① 新输入被接受（`submit`）② `matter_revision` 写入（`understandingChanged`）③ 群聊"记录但不回复"的消息（`RawEventProcessor`） | `wakeConcernsByTerms()`：命中的开放关注 `next_check_at=now`，记 `wake_reason`；不直接联系 | 纯 SQL，0 模型调用 |
| **T3 活动/作品** | `finishThoughtReview` 写入（含 completed） | 若关注关联了同一 `matter`/活动，`next_check_at=now`；`completed` 可生成一个"想分享"的关注（受配置控制） | 1 次信号 + 后续按 T1 处理 |

**协调器优先级**（改 `PrivateReview::wake`）：

```
到期活动 (nextThought(true))
  → 到期关注 (claimDueConcern)
    → 新来源回顾 (claimPrivateReflection)
```
理由：活动和关注都有"到期"，新来源回顾有 24h 窗口、可以补；先处理有时效的，避免回顾额度被新来源吃掉。

**预算**：三者共用 `thought_review_log` 的日上限与冷却；`daily_limit` 只作总闸。到期关注不额外占用配额。

### 4.7 前台写回契约（C2 的核心）

前台注入改为"最多 2 条匹配关注"，并用新的标签与契约：

```
【你自己尚未放下的关注，只读。属于你自己的记录：
 如果本轮主人的话已经回答了它，用 concern_updates 了结；
 如果相关，可以在私下会话里自然提起。群里不要提私人来源。】
[{"ref":"concern/514","topic":"烤肉拌饭","watch":"他说好不好吃、有没有荷包蛋","note":"...","since":"17:48","next_check":"今天早些时候"}]
```

`evidence_decision` 新增：

```json
"concern_updates": [
  {"ref": "concern/514", "action": "resolve", "kind": "answer",
   "evidence_ref": "current_input", "note": ""}
]
```
- `action` 只允许 `resolve` / `keep`。`revise` 留给后台回顾（前台不适合写长 note）。
- `resolve` 必须带 `evidence_ref`，且必须是本轮提供的证据（通常 `current_input`）；**服务端拒绝"无证据了结"**。
- `kind`：`answer`（回答了）/`contradiction`（被否定）/`cancelled`（计划取消）/`dropped`（她自己不想再在意）。
- 提交：与保存轮次同事务（`commitTurnWithConcerns`），失败则整轮回滚并在审计记 `concern_commit=failed`，不用"我记住了"掩盖失败。

**前台释放的两处约束**：
1. `avoid_unprompted_followup` 改为只约束群聊与第三方来源；主人私下会话不受限。
2. 允许用 `assistant_expression` 引用自己的回顾编号（不当作事实依据）；提示词明确列出 `concern/<root>` 是可引用编号。

### 4.8 到期回顾（新 kind = `concern_review`）

上下文对象：

```json
{
  "kind": "concern_review",
  "concern": {"ref":"concern/501","topic":"他的早饭","watch":"他有没有吃、什么时候吃",
              "origin_quote":"我还没吃早饭，等会儿去食堂","own_reflection":"...",
              "created_at":"...","next_check":"...","elapsed_hours":3},
  "since": [ {"turn":503,"text":"...","reported_at":"..."} ],
  "matter": { ...最新认识与版本... },
  "contacts": [ {"timing":"next_chat","outcome":"retained","reason":"..."} ],
  "activity": {"ref":"thought/492","status":"completed","progress":"..."}
}
```

模型输出：

```json
{"concern_action":"keep|revise|resolve|drop|contact",
 "note":"（revise 时）",
 "forget":{"topic":"...","watch":"...","next_check_after_minutes":180},
 "contact":{"timing":"now|next_chat|private","reason":"...","message":"..."}}
```

服务端：`contact` 必须带 `reason`（≤240）与触发依据；`now` 必须带 `message`；`claimReflectionContact` 复用现有投递链路与 `protocol_accepted` 回执；投递后关注保持开放并设 `next_check_at=+2h`（等回答），回答到达后由前台 resolve。

### 4.9 审计与日志

新增日志（全部走 `qInfo`，可在现场 grep）：

```
[Concern] open=N due=M oldest=...
[Concern] match turn=519 refs=concern/514 terms=[好吃,荷包蛋] score=0.83
[Concern] resolved by=519 ref=concern/514 kind=answer
[Concern] due-claimed ref=concern/501 reason=time
[Concern] contact ref=concern/501 outcome=protocol_accepted
[Concern] kept ref=concern/501 next=+180min
```

`cognitive_audit` 新增：
- `kind='concern_match'`（采样，避免每轮写）：`{turn, terms, matched, scores, provided_to_model:true/false}`
- `kind='concern_update'`：`{turn, ref, action, kind, evidence_ref, committed}`
- `kind='concern_review'`：`{ref, wake_reason, elapsed_hours, action, contact_timing, outcome}`

`answer_decision` 审计新增字段（解决"无法区分模型判断错和资料没处理到"）：
```json
{"concerns_provided": ["concern/514"],
 "concern_updates": [{"ref":"concern/514","action":"resolve","kind":"answer"}],
 "concern_commit": "committed",
 "pending_source_count": 3,
 "queue_age_seconds": 1840,
 "understanding_versions": {"m177": 177}}
```

原有 `prior_turn=0` 歧义修复：日志改为
```
[Reflection] retrieval 519 status=complete history=2 prior_found=1 prior_reason=match prior_ref=concern/514
```
其中 `prior_reason ∈ {match, ranked, none, no_open, invisible, superseded}`。

### 4.10 队列可靠性（P0 顺带修）

- 非拒绝类失败加尝试上限（建议 12 次），超过置 `review_required` 并打日志。
- `[Cognition] finish` 同时打印 DB 的 `outcome`（含 `review_required`）。
- 新增只读接口/命令：`--memory-queue-status`，输出按 `outcome/last_error` 分组的计数与最老 `due`。
- 迁移时一次性把卡住的 `review_required` 列出来，而不是只在迁移里重置。

---

## 5. 实施路线

> 估算是单人、熟悉代码库的前提下的人日，含自动测试，不含 7 天现网观察。
> 每一批都必须"可独立回滚"。

### P0 · 接线与回归（5–7 人日）

| 项 | 改动 | 验收 |
|---|---|---|
| P0-1 关注解析器 | `privatereflection.cpp`：新增 `matchedConcerns`/`concernByHead`；`reflectionHistory` 的 prior 改为"匹配优先、检索兜底"；`finishPrivateReflection` 复核改用 `concernByHead` | 用 519/514 的冻结 fixture 重放：`reflectionHistory(519, terms)` 必须返回 `prior=514`；带 `resolve` 能提交成功；`reflection_change` 出现 1 行 |
| P0-2 前台写回 | `llmservice.cpp`：注入匹配关注（≤2）、`concern_updates` 契约、审计字段；`memorymanager`：`resolveConcern/keepConcern`；`commitTurnWithConcerns` | 重放 519：审计 `concern_updates` 有 resolve，DB `state='resolved'`、`resolved_by=519`、`reflection_change` 1 行 |
| P0-3 提示词校准 | 前台去掉 `avoid_unprompted_followup` 的全局限制；注入标签改"她自己的关注"；允许 `assistant_expression` 引用回顾；回顾提示词去掉三处抑制，加入"now 的判定顺序" | 本地 mock 成对回放：同一组 10 个样本，提及/了结率上升且不出现无依据追问 |
| P0-4 可观测性 | `[Concern]` 日志、`prior_reason`、`answer_decision` 新字段、`--memory-queue-status` | 一次真实运行能 grep 出完整链路；`review_required` 可见 |
| P0-5 队列上限 | `matterstore.cpp`：非拒绝类失败上限 12；日志打印 DB outcome | 现有 179 号来源在 12 次内进入 `review_required` 并可查 |

**P0 完成定义**：场景 B（收尾）在**受控重放**下通过；519 的历史数据重放不再产生新的重复 next_chat；19+2 个 CTest 全绿。

### P1 · 关注成为一个有生命周期的对象（10–14 人日）

| 项 | 改动 | 验收 |
|---|---|---|
| P1-1 v23 迁移 | 上述 DDL + 回填 + 迁移测试 | v22→v23 重开不丢数据；v23→v22 回滚仅忽略新列 |
| P1-2 到期回顾 | `claimDueConcern`/`finishConcernReview`/`deferConcernReview`；协调器优先级；`concern_review` 提示词；预算复用 | 到期关注在没有新消息时被领取并产生 `keep/revise/resolve/contact` 之一；48h 内必有处理 |
| P1-3 触发器 | T2：`submit` 后 `wakeConcernsByTerms`；`understandingChanged` 挂接；`RawEventProcessor` 群消息挂接。T3：`finishThoughtReview` 发信号 | 新证据到达后 60s 内关注被标记到期；无命中不触发 |
| P1-4 主动联系返工 | 回顾/到期回顾的 `contact` 契约；`claimReflectionContact` 记录 attempts；`canContactNow` 与写作解耦（`proactive.enabled` 即可）；投递日志与 outcome | 一次 `now` 完整走通到 `protocol_accepted`；重复投递 0；无理由不投递 |
| P1-5 next_chat 消费 | 前台注入匹配关注时带上 `contact_timing/outcome`；提示词允许自然提起；多关注（≤2） | 主动聊起时能说出"上次你说…我还没问"；不是每次都说 |

**P1 完成定义**：场景 A（主动）在受控回放 + 一次真实 QQ 下通过；负向对照（安静日）0 条主动联系；两个对称场景都能跨重启复现。

### P2 · 归因与可追溯（6–9 人日）

| 项 | 改动 | 验收 |
|---|---|---|
| P2-1 发起者/承诺方 | `matter_revision` 增加 `initiator`/`promisor`（owner/mako/空）+ 服务端枚举校验 + 提示词规则；`semantics` 保持兼容 | "茉子请你之后告诉她"记成 `initiator=mako, promisor=''`；"你答应我"记成 `promisor=owner`；两类有测试样本 |
| P2-2 主动消息进历史 | 提醒/联系/分享写 `chat_history(turn_kind='proactive')` + `raw_events(direction='out')`；`promptassembler` 已有渲染分支可直接用 | 下次问"你刚才为什么找我"，她能引用那条主动消息作答 |
| P2-3 视觉观察留存 | 观察结果写 `observation`（新表或 `raw_events` 扩展）并挂 turn；检索可读 | 隔天/换渠道问"昨天那张图是什么"，能按权限回答 |
| P2-4 管理员可见性 | 把 PR-5 并进设置页"今天"面板：开放关注、到期、最近联系、队列积压 | 面板能显示 P0/P1 的审计数据 |

### P3 · 收尾（3–5 人日）

- 检索性能：关注匹配若因开放关注增多变慢，加索引或把候选集剪到 14 天；**不引入向量匹配**。
- 场景 fixture：`tests/fixtures/concern_carried_answer.json`（B）、`concern_due_contact.json`（A）。
- 文档：`ARCHITECTURE.md`（v23 数据流）、`STATUS.md`（B 阶段状态）、`ROADMAP.md`（B2/B3/B4 勾选）、`CONFIG_REFERENCE.md`（`proactive.*` 语义修正）。
- 清理：`RETRIEVAL_REFACTOR_P2.md` 的 legacy 回退描述（代码已无）、`memoryretrieval.cpp` 死代码。

---

## 6. 关键验收脚本

### 6.1 场景 B：答案已经给了（对称场景 2）

**数据前置（冻结自 11/22 真实数据，脱敏）**

| turn | 时间 | 内容 | 预期 |
|---|---|---|---|
| 514 | 17:48 | 主人「烤肉拌饭终于到啦」 | 回顾产生关注 `topic=烤肉拌饭`，`watch=他说好不好吃、有没有荷包蛋`，`next_check=+6h` |
| 519 | 18:54 | 主人「很好吃哦，香香的油油的，没有荷包蛋呀。要是茉子也能和我一起吃该多好」 | 前台 `concern_updates=[{ref:concern/514, action:resolve, kind:answer, evidence_ref:current_input}]` |

**步骤与预期**

1. **转 519**：审计出现 `concerns_provided=[concern/514]`；保存成功后 `private_reflection(514).state='resolved'`、`resolved_by=519`、`resolved_kind='answer'`；`reflection_change(turn=519,prior_turn=514,action=resolve)` 1 行。
2. **重启进程**：`matchedConcerns` 不再返回 514；`claimPrivateReflection` 不会为 514 再建新行。
3. **第二天 / 换到 pet 渠道**：主人发「早上好」。提示词里**不得**出现「好不好吃/荷包蛋」的开放关注；她不得再问同一问题（人工判读 2 个样本）。
4. **被问起**：主人问「我昨天晚饭吃的什么？」→ 回答引用 `chat/519`，不得说"没听你说过"。
5. **负向（模糊回答）**：把 519 换成「还行吧」。契约要求 resolve 必须带 `current_input` 证据；模型若不 resolve，关注保持开放且 `next_check_at` 顺延；**不得**因为"没词"而误判了结。自动测试用本地 mock 锁契约，实机只观察 2 个样本。

**自动测试（新增）**
- `concernCarriedAnswerResolvesAcrossRestart`（DB + 协调器，临时库）
- `answeredConcernIsNotReinjected`（前台 prompt 断言）
- `resolveRequiresCurrentInputEvidence`（拒绝无证据 resolve）
- `ambiguousAnswerKeepsConcernOpen`（契约层）

### 6.2 场景 A：有进展时主动（对称场景 1）

**A1 · 时间到期（主用例）**

1. Day1 08:00 主人：「我还没吃早饭，等会儿去食堂」→ 回顾产生关注 `topic=早饭`，`watch=他有没有吃/什么时候吃`，`next_check_after_minutes=180`。
2. 08:05–11:00 无新消息。11:00 到期回顾领取，`since` 为空，`matter` 仍为"未吃"。
3. 模型 `concern_action=contact, timing=now, reason="他说没吃早饭，现在快中午了"，message=...`。
4. 断言：`reflection_contact(timing=now,outcome=protocol_accepted)`；日志 `[Concern] contact`；60s 内无第二条；关注保持开放并 `next_check=+2h`。
5. 主人回复「吃啦」→ 前台 resolve（回到场景 B 的路径）。

**A2 · 新证据唤醒（次用例）**

1. 关注「快递到了吗」`next_check=+8h`。
2. 主人在 pet 渠道说「快递到了，晚点拆」（新消息但非提问；或群聊"记录但不回复"消息）。T2 命中 → `next_check_at=now`，审计 `wake_reason=new_evidence`。
3. 60s 内到期回顾看到该证据 → `contact now`，理由是"你之前问的快递到了"。
4. 断言：事件到投递 <5 分钟；理由包含新证据来源；不重复。

**A0 · 负向对照（必须有）**

- 安静日：有关注但无新证据、未到期 → 24h 内 0 条主动联系；到期回顾一律 `keep` 或 `wait`。
- 这条用于防止"放松提示词导致无理由联系"。

**自动测试（新增）**
- `dueConcernTriggersContactReview`（mock 模型返回 contact now，断言投递与状态）
- `newEvidenceWakesConcern`（wakeConcernsByTerms）
- `noEvidenceDoesNotContact`（负向）
- `contactRespectsQuietWindowAndTargets`

### 6.3 工程回归

- `ctest` 全绿（当前 19 套，新增 ≥4 套：`tst_concern`、扩 `tst_memorymanager`/`tst_memoryagent`）。
- 现有回归必须保持：`tst_memorymanager` 的 `independentReflectionRespectsRestAndSourceLifetime`、`reflectionReconsideration`；`tst_memoryagent` 的 `coordinatorArbitratesWritingAndIncomingTurn`。
- **成对回放**：P0-3 的提示词改动必须用同一组样本做"改前/改后"对照，只看"是否出现无依据追加义务/重复追问"，不看引用数。

---

## 7. 风险与取舍

| 风险 | 后果 | 缓解 |
|---|---|---|
| 变得过度主动/骚扰 | 体验倒退，比不主动更糟 | 三类触发器 + reason 必填 + 冷却 + 负向对照；主动数不作 KPI |
| 误判"已了结" | 真正的关心被过早关闭 | resolve 必须带 `current_input` 证据；`keep` 是默认；审计可回放 |
| 匹配误唤醒 | 无关关注被注入，提示词进一步被忽略 | 阈值 0.6 + 最多 2 条 + 同 matter 加权；前台允许"不提及" |
| 事务不一致 | 回复说"知道了"但状态没变 | turn + concern 同事务；失败回滚并审计 |
| 迁移风险 | 现有 33 条回顾状态丢失 | 只加列/回填，不重建 `private_reflection`；`reflection_change` 0 行才重建 |
| 性能 | 每轮多一次 SQL | 候选 ≤24 行、纯词面、无 embedding、无额外模型调用 |
| 提示词回归 | 表达质量下降 | 成对回放 + 只改必要句；保留 `avoid_unprompted_followup` 的群聊部分 |
| 数据膨胀 | 关注永不结束 | 14 天保留期 + `resolved/dropped` 终态 + 到期回顾必须给动作 |

---

## 8. 排期

| 批次 | 人日 | 里程碑（可演示） |
|---|---|---|
| P0 | 5–7 | 519/514 重放通过；场景 B 受控通过 |
| P1 | 10–14 | 场景 A1/A2 受控通过；一次真实 QQ 主动联系 |
| P2 | 6–9 | 主动消息可回认；视觉可跨轮；管理面板 |
| P3 | 3–5 | 文档与性能收尾 |
| 现网观察 | 7 天 | 指标表（§3.5）达标 |

总计约 5–6 周（单人），其中 P0+P1 就能覆盖两个对称验收。

---

## 9. 与现有文档/编号的关系

- 本文对应 `ROADMAP.md` 的 **B2/B3/B4**，不新增阶段字母；`B5` 连续运行验收沿用。
- `STATUS.md` 的"下一步：到期重新考虑普通关注、活动结果触发"正是 P1；本文给出其可执行形态。
- `ARCHITECTURE.md` 的"v22 回顾先检索…再读取相关旧关注"需要改为"先解析相关关注，再用检索核实原话"，否则文档会继续描述当前的缺陷设计。

---

## 10. 需要你决定的问题

1. **关注保留期**：14 天？还是"直到 resolve/drop"（可能永不结束）？
2. **安静时段**：现在 `23:59–00:00` 几乎等于全天开放。你希望她能在哪些时段主动联系？要不要默认 `22:30–08:00` 静默？
3. **`now` 的默认额度**：日上限内不单设，还是给主动联系单独的小额度（例如每天 ≤2 条）以防止骚扰？
4. **收尾判据**：是否接受"模型自报 + 引用 current_input"作为唯一判据？我建议接受，但保留审计与"恢复开放"的手动入口。
5. **发起者/承诺方**是否纳入本次：我建议放 P2，不阻塞 B 验收；如果你更在意"别把她的请求记成我的承诺"，可以提到 P1。
6. **写作完成**是否自动生成"想分享"的关注，还是保持"仅 share_with_owner"？我建议先保持现状，避免把主动性绑在写作上。
7. **现有 33 条开放关注**：迁移时是否回填 `next_check_at`（给它们一次再进入机会），还是全部视为"仅新证据触发"？

---

## 附录 A · 复核所用查询

```sql
-- 关注与联系分布
select state,count(*) from private_reflection group by 1;
select timing,outcome,count(*) from reflection_contact group by 1,2;
select count(*) from reflection_change;
select * from reflection_basis;

-- 关键轮次
select id,timestamp,user_input from chat_history where id in (359,464,514,519,520);
select turn_id,state,version,note from private_reflection where turn_id in (514,519);

-- 积压
select id,attempts,last_error,outcome,due from cognitive_source order by id desc limit 5;

-- 审计是否提供/引用回顾
select id,recorded,data from cognitive_audit where kind='answer_decision' order by id desc limit 8;
```

## 附录 B · 关键文件索引

| 关注 | 文件 |
|---|---|
| 关注存储与选取 | `src/memory/privatereflection.cpp`（`reflectionHistory` @90、`recentPrivateReflection` @291） |
| 回顾调度 | `src/cognition/llmservice.cpp`（`requestPrivateReview` @1111、`generatePrivateReview` @1170） |
| 前台注入 | `src/cognition/llmservice.cpp:707-714` |
| 协调器 | `src/app/cognitivecoordinator.cpp`（`reviewIfDue` @56、`submit` @71） |
| 回顾落库 | `src/app/privatereview.cpp`（`wake` @10、`receive` @40） |
| 投递 | `src/app/appcontroller.cpp`（reminder @211、contact @231、share @264、`canContactNow` @427） |
| 认识更新 | `src/memory/matterstore.cpp`（`pendingObservation` @68、重试 @173、提交 @236-298） |
| 活动 | `src/memory/memorymanager.cpp`（`nextThought` @265、`finishThoughtReview` @326） |
| 审计 | `src/cognition/llmservice.cpp:2533-2622`、`src/cognition/evidenceaudit.h` |
| 测试 | `tests/tst_memorymanager.cpp:819-951`、`tests/tst_memoryagent.cpp:571-643,769-873`、`tests/tst_concern.cpp`（新增） |

---

## 11. P0 落地记录（2026-11-22 23:40）

### 11.1 已落地

| 方案项 | 改动 | 文件 |
|---|---|---|
| P0-1 关注解析器 | `matchedConcerns`（note 的 2/3 字连续片段 × 整条当前输入；3 字命中或 2 个 2 字命中才算相关；可按"早于某轮"过滤）、`concernByHead`；`reflectionHistory` 改为"匹配优先、ranked 兜底"，返回 `prior_reason` | `src/memory/privatereflection.cpp`、`src/memory/memorymanager.h` |
| P0-2 前台写回 | `resolveConcern`（事务：校验 head 开放/可见/依据、`chat/N` 证据可读、写 `concern_resolution`、复制 basis、置 `state='resolved'`）；前台解析 `evidence_decision.concern_updates` 并提交，失败留审计 | `src/memory/privatereflection.cpp`、`src/app/appcontroller.cpp` |
| P0-3 提示词校准 | 前台注入最多 2 条"她自己的关注"并加入 `concern_updates` 契约；`avoid_unprompted_followup` 只约束群聊/第三方；回顾提示词去掉"无需联系任何人"，改为"有新证据/到约定时间/活动有结果才 now" | `src/cognition/llmservice.cpp` |
| P0-4 可观测性 | `[Concern] provided/resolved/update_failed` 日志；`prior_found/prior_reason/prior_ref`；审计新增 `concerns_provided`、`pending_source_count`、`queue_oldest_due_age_seconds`、`concern_update`；`--memory-queue-status` | `llmservice.cpp`、`appcontroller.cpp`、`main.cpp`、`matterstore.cpp` |
| P0-5 队列上限 | 只对不可恢复原因（协议/非法 JSON/截断/证据基线冲突）计 12 次死信；网络/存储/证据变化不判死；`[Cognition] dead_letter` 日志 | `src/memory/matterstore.cpp` |
| 静默轮写回 | 模型 `close/defer` 空句时也提交 `concern_updates`（新增 `LLMService::concernUpdatesReady` → 协调器落库） | `llmservice.h/.cpp`、`cognitivecoordinator.cpp` |
| 测试 | 新增 `tests/tst_concern.cpp`（10 用例，覆盖无检索命中的 prior、前台了结、无证据拒绝、重复了结、回顾链 revise/resolve、无关/常见词负例）；`tst_memorymanager` 的 `reflectionReconsideration` 断言按新契约更新 | `tests/` |

### 11.2 与方案的两处偏离（有意）

1. **了结流水单独建表 `concern_resolution`，不复用 `reflection_change`。** 对抗审查发现两者共用 `turn` 主键：同一轮先被前台了结、后再被回顾修订时会永久 `defer`。`reflection_change` 现在只服务回顾链，`concern_resolution(id PK, turn, prior_turn UNIQUE, action, kind)` 服务对话写回。
2. **结构版本仍为 22。** 新表用幂等 `CREATE TABLE IF NOT EXISTS` 补齐，不改 `PRAGMA user_version`；避免大面积改既有版本断言。正式 v23（`root_turn/topic/watch/next_check_at/...`）留给 P1 一次性迁移。

### 11.3 验证

- 全量构建：`cmake --build . -j 8`（含 `Windows_AI`、20 个测试目标）零编译错误。
- `ctest`：20 套件 19 通过；`tst_runtimelog` 失败属**环境性**——该用例 `QProcess` 起子进程后读管道 stdout，宿主进程沙箱下管道不可用（手动把子进程 stdout 重定向到文件可看到 `ready`）；该测试只编译 `runtimelog.cpp`，与本轮 diff 无关。
- 新增 `tst_concern` 10/10；`tst_memorymanager` 66 passed + 1 skipped（`MAKO_REAL_DB` 未设置）；`tst_memoryagent` 通过。
- 519/514 场景：`reflectionHistory(519, 1, {}, {"好吃","荷包蛋",...})` 返回 `prior.source_turn=514`、`prior_reason=match`；`resolveConcern(514, 519, "answer", "current_input")` 成功后 `state='resolved'` 且 `concern_resolution` 恰 1 行，后续注入不再出现。

### 11.4 P0 仍缺（P1 必做）

- 到期回顾（`claimDueConcern`）与 `next_check_at`：主动联系仍是 0 次的根因之一。
- `topic/watch/root_turn` 字段：匹配目前只靠 `note` 文本，无显式主题/待验证点。
- 新证据/活动完成触发器（T2/T3）。
- `resolveConcern` 不校验 ref 属于本轮注入集合（幻觉可关闭可见关注）；无 reopen 路径。
- `AppController` 仍未编入任何测试目标，`concern_updates` 的端到端只有 DB/协调器层覆盖。
- 交接细节见 [Agent 工作记录](AGENT_WORKLOG.md)。
