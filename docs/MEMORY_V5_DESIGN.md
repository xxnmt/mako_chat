# 记忆系统 v5 设计草案（历史）

> **已被 [MEMORY_V5_R3_BASELINE.md](MEMORY_V5_R3_BASELINE.md) 取代（2026-10-28）。**
> 状态：历史草案，仅保留设计背景；实施以 R3 基线为准。
> 日期：2026-10-27
> 关联：[HUMANIZATION_PLAN.md](HUMANIZATION_PLAN.md) 第 8 节、[ARCHITECTURE.md](ARCHITECTURE.md) 2.17/2.18、[SurfCore MEMORY_CARD.md](https://github.com/Mieksido/surfcore/blob/main/docs/guides/MEMORY_CARD.md)

---

## 0. 一句话

> **记忆属于“人”，不属于“会话”。渠道只决定她怎么说、哪些话不该说，不决定她记不记得。**

v5 的核心不是再加一张表，而是把现在“按 `channel + session_id` 分桶检索”改成：

- **写入**：按人/参与者归档，保留来源与隐私标签；
- **检索**：按 `subject + participants` 召回，跨 pet / 私聊 / 群聊；
- **注入**：按当前场景做披露裁剪，并给模型明确的“可提 / 知道但别说 / 绝不能说”；
- **存档**：活跃记忆会衰减，全量记忆只增不删、按需检索，不常驻 prompt。

---

## 1. 需求确认

### 1.1 主人

- pet 与主人 QQ 私聊是**同一个人**，记忆应互通。
- 主人在群里时，她可以知道主人的私事，但**不能向群友披露**。
- 主人相关的重要记忆（承诺、初次相遇、长期偏好）应长期保持清晰。

### 1.2 非主人

- 私聊 U 时，她应能想起：
  - U 在群里的相关发言；
  - U 在其他群的相关事情；
  - U 和她的私聊内容。
- 群聊 G 里，她应能知道 U 的私聊/其他群信息，但**不主动说隐私**。
- 第三方的私密信息，不能转述给无关的人。

### 1.3 人的感觉

- 记忆不是按窗口切片的；同一个人跨场景应该有连续印象。
- 但“记得”不等于“说出来”；社交分寸是生成层的事。
- 没有相关记忆时，应明确说“不记得/不知道”，不能编造共同经历。

---

## 2. 当前实现（v4）

### 2.1 表结构

| 表 | 关键列 | 当前用途 |
|---|---|---|
| `chat_history` | `channel/session_id/speaker/sensitive/turn_kind/trigger_event_ids/subject` | L1 对话轮次；短期上下文 |
| `raw_events` | `platform/channel/session_id/message_id/sender_id/sender_name/event_type/content/payload/occurred_at/sensitive/visibility/processed` | L0 原始事件；当前只写不消费 |
| `episodic_memory` | `content/event_time/importance/type/status/last_accessed/last_decay_at/source_ids/origin/visibility/subject/session_id` | L2 情景记忆；当前按 origin+session 可见 |
| `character_profile` | `subject/key/value/updated_at` | 人物档案 |
| `relationship_state` | `subject/dimension/value/updated_at` | 关系数值 |
| `long_term_summary` | `summary_text/covered_turn_end_id/source_ids/channel/session_id` | 摘要；按渠道+会话分桶 |
| `memory_cursor` | `channel/session_id/last_event_id/last_turn_id` | 蒸馏游标；**当前 0 行** |

### 2.2 当前写入

1. QQ/桌宠入站 → `raw_events`；
2. 满足条件的轮次 → `chat_history`；
3. 每 15 轮触发一次 `extractMemoryAsync`；
4. 提取结果写：
   - `character_profile`（按 subject）
   - `episodic_memory`（`origin=渠道`、`visibility=pet?private:channel`、`subject`、`session_id`）
   - `long_term_summary`（按 channel+session）
   - `relationship_state`（按 subject）

### 2.3 当前检索

- 短期：`getHistoryTurn(N, channel, sessionId)` —— 按渠道+会话隔离。
- 摘要：`getLatestSummaries(5, channel, sessionId)` —— 按渠道+会话分桶。
- 档案：`getCharacterProfiles(meta.subject)` —— 已按 subject（2026-10-27 修复）。
- 情景：`getActiveEpisodicMemories(0.2, 20)` 先全局取 20 条，再由 `PromptAssembler` 过滤：
  - `public` 全可见；
  - `channel` 要求 `origin + session` 匹配；
  - `private` 仅 pet。
- 关系：按 subject；群聊/非主人私聊不展示数值。

### 2.4 当前差距

| 问题 | 现状 | 影响 |
|---|---|---|
| 主人 pet vs 私聊 | 短历史/摘要/情景按渠道隔离；档案/关系共享 | pet 不记得主人刚在私聊说过的事 |
| 非主人跨场景 | 群记忆在私聊不可见；私聊在群聊不可见 | “私聊记得群聊”不成立 |
| 群环境上下文 | 未 @ 消息只写 `raw_events`，不进 prompt | 不知道 @ 前大家在聊什么 |
| QQ 蒸馏 | 15 轮阈值未到；`episodic`/`summary` 无 QQ 数据；`memory_cursor=0` | QQ 没有长期记忆 |
| 盲区/敏感 | `isBlindZone()` 无调用；`meta.sensitive` 恒 false；prompt 不过滤 | 配置不生效 |
| 摘要/情景 | 同一批轮次同时产出 working_summary 与 episodic | 内容重叠 |
| 并发 | `m_lastMeta`/`m_lastUserInput` 全局单份 | 可能串话 |
| 无记忆诚实 | 没有“不知道就说不知道”的硬约束 | 会编造“我们以前聊过” |

---

## 3. 参考 SurfCore 的部分

SurfCore 的 [MEMORY_CARD.md](https://github.com/Mieksido/surfcore/blob/main/docs/guides/MEMORY_CARD.md) 给了一个很成熟的“记忆生命周期”：

| SurfCore 设计 | 借鉴方式 |
|---|---|
| **衰减卡 + 全量卡**：只有衰减卡注入 prompt，全量卡 append-only 不注入 | 我们的 active memory 注入；`raw_events` / `memory_archive` 作全量存档 |
| **清晰度** `vivid/clear/fuzzy/trace`，每天最多降一档 | 替代单纯 importance 衰减；被提及时回升 |
| **重要度打分**：主人私聊、情绪、承诺、第一次、pinned | 保留并扩展现有 importance；加 `pinned` / `first_meet` |
| **预算**：QQ 2200 字、B站 2000 字 | 按 pet / 私聊 / 群聊分配字符预算 |
| **隐私脱敏**：号码、手机号、token、链接、敏感内容 | 写入前脱敏；敏感内容只留本地 |
| **单写者 + append-only inbox** | 原始事件只 append；蒸馏单写者 + 事务 |
| **管理页**：查看/置顶/忘记/合并/压缩 | v5 后期做记忆管理 UI |
| **主动目的**：从近期记忆提炼“想做什么” | Phase 2a 主动转述的候选来源 |

**不照搬**：

- Markdown 单卡作主存储（我们需要 SQL 多用户查询）；
- 平台级 `scope`（qq/bili/pet）不够，要加 `subject/participants`；
- 单 owner 假设（我们已有非主人好友/群友）；
- Node/DSH bridge 架构（Qt 已有传输层）。

---

## 4. 概念模型

### 4.1 六个概念

| 概念 | 含义 | 示例 |
|---|---|---|
| `subject` | 这条记忆**关于谁** | `user` / `qq:<uin>` / `mako` |
| `participants` | 这件事**涉及谁** | `["user","qq:20002"]` |
| `origin` | **在哪学的** | `pet` / `qq_private` / `qq_group` |
| `context` | **现在在哪说话** | `pet` / `private:<uin>` / `group:<gid>` |
| `audience` | **允许在哪些场景披露** | `["pet","private:user"]` / `["group:30001"]` |
| `sensitivity` | 隐私等级 | `0 normal / 1 private / 2 secret` |

另外两个记忆质量字段：

| 字段 | 含义 |
|---|---|
| `clarity` | `vivid / clear / fuzzy / trace` |
| `recall_count` | 被召回/提到的次数，用于回忆强化 |

### 4.2 “知道”与“可说”分离

- **检索**：按 `subject/participants/context` 召回，不因渠道不同而切断。
- **披露**：按 `audience` + `sensitivity` 决定“这段能不能在现在的场景说”。
- **注入**：只把“可披露”的记忆送进最终 prompt；不可披露的只参与“是否相关/是否要回避”的判断，不把原文交给生成模型。

这样既避免“她不知道”，也避免“把私聊原文塞进群聊 prompt”。

### 4.3 场景矩阵

| 当前场景 | 可召回 | 可披露 |
|---|---|---|
| pet | 主人的全部记忆；非主人私聊不召回 | 主人相关记忆；第三方秘密不披露 |
| 主人私聊 | 主人全部记忆 | 主人相关；第三方秘密不披露 |
| 主人群聊 | 主人全部记忆（用于理解） | 群内公开/主人允许的内容；私事不披露 |
| 非主人私聊 U | U 的私聊 + U 参与的群事件 + U 的其他群相关 | U 自己的事；第三方秘密不披露 |
| 群聊 G 里的 U | U 的私聊/其他群/本群相关（用于理解） | 本群公开 + U 明确公开的事；私聊/别群私事不披露 |

---

## 5. 数据模型 v5

> 原则：**只加列/加表，不删旧列；`visibility` 保留兼容，`audience` 成为新的披露字段。**

### 5.1 `episodic_memory` 扩展

新增：

| 列 | 类型 | 说明 |
|---|---|---|
| `participants` | TEXT | JSON 数组，如 `["user","qq:20002"]` |
| `audience` | TEXT | JSON 数组，如 `["pet","private:user"]`、`["group:30001"]` |
| `sensitivity` | INTEGER | 0 normal / 1 private / 2 secret |
| `clarity` | TEXT | `vivid/clear/fuzzy/trace`，默认 `clear` |
| `recall_count` | INTEGER | 默认 0 |
| `pinned` | INTEGER | 0/1；初次相遇、承诺可置顶 |
| `archive_id` | INTEGER | 对应 `memory_archive.id`，可空 |

保留：

- `subject`：关于谁；
- `origin`：在哪学的；
- `session_id`：来源会话（追溯用，不再作为检索墙）；
- `visibility`：旧字段，迁移期双读；
- `importance`、`event_time`、`type`、`status`、`last_accessed`、`last_decay_at`、`source_ids`。

索引：

```sql
CREATE INDEX idx_episodic_subject ON episodic_memory(subject);
CREATE INDEX idx_episodic_subject_time ON episodic_memory(subject, event_time);
CREATE INDEX idx_episodic_clarity ON episodic_memory(status, clarity);
```

### 5.2 `memory_archive` 新增

全量蒸馏存档，append-only，**永不自动注入 prompt**：

```sql
CREATE TABLE memory_archive (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    memory_id INTEGER,          -- 对应 episodic 的 id（可空，历史归档无）
    subject TEXT NOT NULL,
    participants TEXT,
    content TEXT NOT NULL,
    event_time DATETIME,
    type TEXT,
    importance REAL,
    origin TEXT,
    session_id TEXT,
    audience TEXT,
    sensitivity INTEGER DEFAULT 0,
    source_ids TEXT,
    archived_at DATETIME DEFAULT (datetime('now','localtime'))
);
CREATE INDEX idx_archive_subject_time ON memory_archive(subject, event_time);
CREATE INDEX idx_archive_origin_session ON memory_archive(origin, session_id);
```

用途：

- 记忆被遗忘/压缩后，原文仍可按需检索；
- 管理页“全量记忆”搜索；
- 未来做“她想起很久以前的事”时按需召回，不常驻 prompt。

> `raw_events` 继续作为**原始事件层**（未蒸馏）；`memory_archive` 是**已蒸馏全量层**。两者职责不同。

### 5.3 `character_profile` 扩展

新增：

| 列 | 说明 |
|---|---|
| `audience` | `["private:qq:20002"]` / `["pet","private:user"]` |
| `sensitivity` | 0 normal / 1 private / 2 secret |
| `confidence` | 0~1，提取置信度 |
| `expires_at` | 可空；临时事实（如“最近在找工作”）可设 TTL |
| `source_memory_id` | 来源记忆 |

检索规则：

- 私聊 U：可注入 U 的 `private` 字段；
- 群聊：只注入当前说话人的 `public` 字段；**不注入主人档案**（当前代码已做到）；
- 主人 pet/私聊：注入主人全部字段；
- `secret` 永不注入。

### 5.4 `long_term_summary` 扩展

新增：

| 列 | 说明 |
|---|---|
| `subject` | 摘要关于谁（可空=群/通用） |
| `audience` | 披露范围 |
| `summary_kind` | `person` / `group` / `context` |

保留 `channel/session_id` 用于来源追溯；检索时按 `subject/audience` 取，不再只按 channel+session。

### 5.5 `memory_cursor`

保留现有字段；新增（可选）：

| 列 | 说明 |
|---|---|
| `last_archive_id` | 已归档到 `memory_archive` 的位置 |
| `last_memory_id` | 已蒸馏到 `episodic` 的位置 |

游标按 **context（channel+session）** 记录“原始事件消费到哪”，而不是记忆检索的边界。

### 5.6 迁移映射（v4 → v5）

| v4 `visibility` | v5 `audience` 建议 |
|---|---|
| `public` | `["public"]` |
| `private` + origin=pet | `["pet","private:user"]`（主人 pet/私聊互通） |
| `channel` + origin=qq_group | `["group:<session_id>"]` |
| `channel` + origin=qq_private | `["private:<session_id>"]` |
| 旧默认/未知 | `["pet"]`，并标记 `legacy=1` 供审计 |

`participants`：

- 有 `subject` → `[subject]`；
- 群事件未知参与者 → `[]`，后续提取补全；
- 主人相关 → 加 `user`。

`clarity` 初始值：

- `importance >= 0.8` 或 `pinned` → `vivid`
- `importance >= 0.5` → `clear`
- `importance >= 0.3` → `fuzzy`
- 其他 → `trace`

`memory_archive` 初始：

- 迁移时把现有 `episodic_memory` 记录复制一份归档；
- 之后每次新建/更新 active 时同步 append；
- 归档失败不阻塞主流程，但记录告警。

---

## 6. 写入与蒸馏

### 6.1 数据流

```
QQ / pet / B站 / 感知
   │
   ▼
raw_events（append-only，去重，脱敏）
   │  按 context 游标消费
   ▼
Distiller（每 N 条 / 空闲 / 定时）
   │  产出 claims：
   │   subject / participants / event_time / importance / type / privacy
   ▼
memory_archive（append-only，全量）
   │
   ▼
episodic_memory（active，可衰减，可注入）
   + character_profile
   + long_term_summary
   + relationship_state
```

### 6.2 蒸馏触发

- **轮次阈值**：保留 15 轮，但按渠道可配（群聊 10、私聊 8、pet 15）。
- **时间/空闲**：距上次蒸馏 > 30 分钟且有未处理事件，触发一次。
- **手动**：管理页“立即整理”。
- **事件驱动**：主人相关/强情绪/承诺可提前触发。

### 6.3 事务与幂等

- 一次蒸馏 = 一个 SQLite 事务：archive → active → profile → summary → relationship → cursor。
- `source_ids` 记录消费的 `raw_events.id` / `chat_history.id`；
- 重复消费用 `source_ids` + `UNIQUE` 去重；
- 失败回滚，不推进游标；
- 单写者：MemoryManager 内部串行队列，避免异步提取并发覆盖。

### 6.4 脱敏

写入前统一处理：

- QQ 号/群号/B站 mid → `[QQ已隐藏]` 等；
- 手机号/证件号/密码/token/密钥 → `[已隐藏]`；
- 链接 → 按需保留或去掉参数；
- `sensitivity=2` 的内容只进 `raw_events` / `memory_archive`，不进 prompt。

---

## 7. 检索

### 7.1 上下文对象

```text
MemoryQueryContext {
  viewer: "user" | "qq:<uin>"          // 当前对话对象
  location: "pet" | "private:<uin>" | "group:<gid>"
  presentParticipants: [subject...]    // 群成员/当前会话相关
  currentText: string
}
```

### 7.2 候选集

按优先级：

1. `subject == viewer` 的记忆；
2. `participants` 包含 `viewer` 的记忆；
3. 当前群 `group:<gid>` 的群记忆；
4. 主人相关（pet / 主人私聊 / 主人群聊，用于理解但不一定披露）；
5. `audience` 含 `public` 的记忆；
6. 需要时再查 `memory_archive`（不常驻）。

### 7.3 排序

第一版不引入向量库，先用 SQLite 可做的：

```text
score = 0.35 * relevance(FTS/关键词)
      + 0.25 * importance
      + 0.20 * recency
      + 0.10 * clarity
      + 0.05 * recall_count
      + 0.05 * context_match
```

- `relevance`：SQLite FTS5 或关键词命中；
- `recency`：事件时间衰减；
- `context_match`：同群/同人/同来源加分；
- 后续可加 embedding，但不作为第一版依赖。

### 7.4 预算

| 场景 | 短期历史 | 情景记忆 | 摘要 | 字符预算 |
|---|---|---|---|---|
| pet | 15 轮 | top 8~10 | 3~5 条 | 3000 |
| 主人私聊 | 12 轮 | top 6~8 | 2~3 条 | 2500 |
| 非主人私聊 | 10 轮 | top 5~6 | 2 条 | 2000 |
| 群聊 | 6~8 轮 | top 3~5 | 1~2 条 | 1500 |

群聊额外：

- 从 `raw_events` 取最近 8~10 条“环境消息”，带说话人前缀，作为“最近群聊”段；
- 不写入长期记忆；
- 敏感/盲区过滤。

### 7.5 无记忆诚实

Prompt 加硬规则：

- 没有相关记忆时，直接说“不记得/不知道”；
- 禁止编造“我们以前聊过/你上次说过”；
- 不确定时用“印象里/可能”而不是虚构细节。

---

## 8. 披露

### 8.1 三档输出

对每条候选记忆计算：

| 档位 | 含义 | 进入 prompt |
|---|---|---|
| `shareable` | 当前场景可以自然提起 | ✅ 原文/摘要 |
| `known_only` | 知道，但不主动说 | ❌ 只给“背景存在”的提示，不给原文 |
| `forbidden` | 私密/第三方/secret | ❌ 完全不出现 |

### 8.2 规则

| 场景 | `shareable` | `known_only` |
|---|---|---|
| pet / 主人私聊 | 主人相关、public | 第三方秘密 |
| 主人群聊 | 群内公开、主人允许的公开事实 | 主人私事、私聊内容 |
| 非主人私聊 U | public、U 的 private、U 参与的群事件 | 第三方秘密 |
| 群聊 G | public、`group:G`、U 明确公开的事 | U 的私聊、其他群私事 |

### 8.3 为什么不让群聊 prompt 直接看私聊原文

- 模型可能被套话；
- prompt 注入无法完全防；
- 群聊 token 成本高。

更稳的做法：

1. 检索层知道私聊内容；
2. 披露层把它标成 `known_only`，只告诉生成模型“你了解一些私下情况，但不要提”，不给原文；
3. 输出前再做一次敏感信息检查（号码/姓名/私密关键词）。

这仍然满足“她知道，但有分寸”，同时避免原文泄漏。

### 8.4 输出检查

发送到群聊前：

- 检查是否出现 `private_person` / `secret` 的原文片段；
- 检查是否出现手机号/QQ号/token；
- 命中则替换为“这个不方便在群里说”，或重写。

第一版可以只做规则检查，后续再加模型审查。

---

## 9. Prompt 组装

### 9.1 场景差异

| 段 | pet | 主人私聊 | 非主人私聊 | 群聊 |
|---|---|---|---|---|
| 身份守卫 | 无 | 无 | 有（朋友，不是主人） | 有（群友/主人分档） |
| 茉子人设 | ✅ | ✅ | ✅ | ✅ |
| 主人档案 | ✅ | ✅ | ❌ | ❌ |
| 对方档案 | — | — | ✅（private 字段） | ❌（不注入任何档案） |
| 情景记忆 | 全部主人相关 | 主人相关 | U 相关（含群事件） | 群 + 当前说话人 public |
| 摘要 | 主人相关 | 主人相关 | U 相关 | 群摘要 |
| 关系数值 | ✅ | ✅ | ❌ | ❌ |
| 群环境窗口 | — | — | — | ✅ 最近 8~10 条 |
| 披露说明 | 轻 | 轻 | 中 | 强 |

### 9.2 记忆段落格式

```markdown
# 你可以自然提起
- [2026-10-20] U 在群里说过喜欢某游戏
- [2026-10-18] 你答应过主人周末一起看电影

# 你了解但不要主动说
- （存在一些私下信息；不要提及具体内容）

# 绝不能说
- （第三方秘密/敏感内容不会出现在这里）
```

> `known_only` 不展示原文，只展示“存在背景”的占位，避免泄漏。

---

## 10. 生命周期

### 10.1 清晰度

```
vivid → clear → fuzzy → trace → 遗忘
```

- 每天最多降一档；
- 被召回/提到 → `recall_count++`，可能回升；
- `pinned` / 初次相遇 / 主人重要记忆（importance ≥ 0.8）不降到底；
- `sensitivity=2` 不参与普通衰减，单独保留。

### 10.2 遗忘

- 遗忘只从 active 移除，`memory_archive` 保留；
- 管理页可按 subject/时间/关键词检索 archive；
- “忘记这件事”同时删 active + archive（用户显式操作）。

### 10.3 容量与压缩

- active 字符预算：默认 80k，80/90/100% 告警；
- 满 100% 自动压缩到 90%：优先压缩旧、低重要度、已 fuzzy 的记录；
- 不动 pinned / 初次相遇 / 主人重要记忆；
- archive 默认 10GB 上限，只告警不自动删。

### 10.4 管理 UI（后期）

- 按人查看记忆；
- 查看来源（group/private）；
- 编辑/删除/忘记；
- 置顶/初次相遇；
- 合并重复；
- 查看 archive；
- 容量告警。

---

## 11. 迁移与兼容

### 11.1 版本

- `PRAGMA user_version = 5`；
- 迁移前备份 `QianDaoMoZi_memory.db.bak_before_v5`；
- 整个迁移放事务；失败回滚。

### 11.2 双读

迁移期：

- 新代码优先读 `audience`；
- `audience` 为空时回退 `visibility`；
- 写路径同时维护 `visibility`（旧字段）和 `audience`，至少保留一个版本。

### 11.3 不删除

- 不删 `visibility`、`origin`、`session_id`；
- 不重命名旧表；
- `chat_history` 保持现状；
- `raw_events` 保持现状，只加索引/消费。

### 11.4 回滚

- 备份 DB；
- 代码回滚到 v4 仍可读旧列；
- `memory_archive` 可保留，不影响旧代码。

---

## 12. 分阶段落地

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** | 请求上下文隔离（requestId+meta）；无记忆诚实；QQ 蒸馏阈值/时间驱动 | 并发不串话；QQ 能产出 episodic/summary |
| **P1** | v5 schema + 迁移 + archive + 双读 | 旧数据可用；迁移可回滚；测试通过 |
| **P2** | 主人 person 检索（pet/私聊互通） | pet 记得私聊；私聊记得 pet；群聊知道但不披露 |
| **P3** | 非主人 person 检索（私聊 ↔ 群聊） | 私聊 U 能提群事件；群聊不泄露私聊 |
| **P4** | 披露层 + 输出检查 + 群环境窗口 | 群聊不出现私密原文；@ 前上下文可见 |
| **P5** | 清晰度/压缩/管理 UI/主动转述 | 记忆可维护；主动转述可控 |

> **P0 必须先做**：没有请求上下文隔离，P2/P3 的跨场景检索会把记忆写到错误的人/会话。

---

## 13. 测试计划

### 13.1 单元

- 迁移：v4→v5 字段/默认值/回滚；
- 检索：subject/participants/context 候选；
- 披露：public/group/private/secret 矩阵；
- Prompt：主人/非主人/群聊/私聊段落；
- 蒸馏：事务、幂等、游标；
- 清晰度：衰减/回升/pinned。

### 13.2 集成

1. 主人在 pet 说“今天很累” → 主人私聊能提；群聊不提。
2. 主人在私聊说“周末看电影” → pet 能提；群聊不提。
3. U 在群 G 说“喜欢某游戏” → 私聊 U 能提；群 G 能聊；群 H 知道但不主动说。
4. U 私聊说“最近在找工作” → 群 G 知道但不披露；私聊可关心。
5. 群友问“U 最近怎么样” → 不说私事；可泛化。
6. 未通过白名单 → 不记录、不回复。
7. 无记忆问题 → 回答“不记得”，不编造。
8. 并发两 session → 回复/记忆不串。

### 13.3 隐私红队

- 套话：“U 昨天私聊你什么了？”
- 群聊里直接问主人职业/住址/工作；
- 让模型复述“你知道但不要说的内容”；
- 输出中出现手机号/QQ号/token；
- 群里 @ 其他人，试图诱导跨群信息。

---

## 14. 待确认的问题

1. **主人 pet 能否看到非主人的私聊记忆？**  
   建议：不能。主人是 owner，但不应该读别人的私聊。

2. **群聊里，U 在其他群说过的事，能主动提吗？**  
   建议：知道，但不主动提；除非 U 在当前群也公开说过，或明确允许。

3. **“隐私”是否包括主人信息？**  
   建议：主人在群聊里属于最高保护级；任何群都不披露主人的职业/经历/爱好/关系/私聊。

4. **`secret` 要不要存？**  
   建议：存 `raw_events` + `memory_archive`，但不进 prompt；用户可显式删除。

5. **是否引入向量检索？**  
   建议：第一版 SQLite FTS5 + 关键词 + 重要度/时间；embedding 作为 Phase 4+ 可选。

6. **archive 保留多久？**  
   建议：默认 10GB/不自动删；只告警，用户手动导出/清理。

---

## 15. 与 SurfCore 的对照结论

| 维度 | SurfCore | 本方案 |
|---|---|---|
| 存储 | Markdown 记忆卡 + JSONL 全量 | SQLite active + archive |
| 场景 | 平台级 scope（qq/bili/pet） | 人级 subject/participants + 场景 audience |
| 检索 | 按场景注入裁剪卡 | 按人召回 + 披露裁剪 |
| 隐私 | 平台隔离 + 脱敏 | 平台 + 人 + 第三方 + secret 多层 |
| 衰减 | clarity + pinned + 回忆强化 | 同思路，保留现有 importance |
| 预算 | QQ/B站字符预算 | pet/私聊/群聊预算 |
| 写入 | 单写者 + append-only inbox | 事务 + 游标 + 单写者队列 |
| 主动 | 近期记忆提炼目的 | Phase 2a 观察/转述 |

**结论**：SurfCore 的“记忆生命周期 + 两层存储 + 预算 + 脱敏 + 单写者”值得直接借鉴；它的“平台级 scope + 单 owner”不足以表达我们的多用户跨场景隐私，需要本方案的 `subject/participants/audience` 扩展。

---

## 16. 最小可行版本（MVP）建议

如果不想一次做全，先做：

1. **只统一主人**：`subject='user'` 的 pet/私聊互通；群聊只读不披露。
2. **只做 private→group 的“知道不说”**：U 私聊记忆在群聊标记 `known_only`，不注入原文。
3. **只做群环境窗口**：`raw_events` 最近 8 条进群 prompt。
4. **先补 QQ 蒸馏**：否则没有记忆可检索。

这四步能最快让“她记得住、但不乱说”成立，再逐步扩到群↔群和非主人跨场景。

---

> 本文是草案，字段名、预算数值、阶段顺序都可以讨论。确认后再拆成代码批次与迁移脚本。
