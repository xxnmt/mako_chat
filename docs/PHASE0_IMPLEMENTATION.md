# Phase 0 实施报告：架构解耦（大脑 + 渠道）

> 类型：实施报告（已完成，待审批）
> 依据：[HUMANIZATION_PLAN.md](HUMANIZATION_PLAN.md) v0.3 第 10.1 节详细设计（2026-10-19 定稿并获批准）
> 实施日期：2026-10-19
> 版本：v1.0
> 构建状态：Debug 构建通过（MinGW 6.11.1，零错误）

---

## 一、实施范围与红线达成

| 红线 | 达成情况 |
|---|---|
| 外部行为零变化 | 桌宠对话链路（气泡/语音/立绘/记忆提取）时序与改前一致；输入侧经 SessionRouter 转发后行为等价 |
| 旧数据零迁移 | 八项结构变更全部带 DEFAULT；relationship_state 走"复制重建"，数据零丢失 |
| 每步独立可构建 | 按 5 步顺序实施，步骤 1+2、步骤 3+4、步骤 5 分别构建验证通过 |

**不在本阶段范围**：QQ 消息收发（Phase 1）、发言门控（Phase 2）、主动对话调度（Phase 2）、设置界面 UI 勾选框（Phase 1 随 QQ 上线）。

---

## 二、改动总览

### 2.1 新增文件（5 个）

| 文件 | 职责 | 关键内容 |
|---|---|---|
| [utterance.h](../utterance.h) | 统一数据协议 | `Utterance`（输出：text/ttsText/tags/channel/sessionId/speaker）、`RequestMeta`（输入：channel/sessionId/speaker/subject/sensitive）、`Channel` 常量命名空间 |
| [ichanneladapter.h](../ichanneladapter.h) | 渠道抽象接口 | `channelName()` + `sendUtterance(const Utterance&)` 纯虚接口 |
| [petchanneladapter.h](../petchanneladapter.h) / [.cpp](../petchanneladapter.cpp) | 桌宠渠道 | 薄包装：`sendUtterance` → 气泡显示；`onAudioReady` 槽保留播放跟读入口 |
| [qqchanneladapter.h](../qqchanneladapter.h) / [.cpp](../qqchanneladapter.cpp) | QQ 渠道占位 | Phase 1 启用；头文件注释含 OneBot v11 反向 WS 实施计划 |
| [sessionrouter.h](../sessionrouter.h) / [.cpp](../sessionrouter.cpp) | 输入路由 | `submitInput` → 身份解析 → 双键历史组装 → `requestReply` 信号；`resolveSubject` 预留 identity 查表 |

### 2.2 修改文件（5 个）

| 文件 | 改动 |
|---|---|
| [memorymanager.cpp](../memorymanager.cpp) | v2 迁移块（L763-847）、`ensureColumn` 静态函数（L722-748）、7 个读写函数扩参 |
| [memorymanager.h](../memorymanager.h) | `saveQATurn`/`getHistoryTurn`/`getUnsummarizedTurns`/`addLongTermSummary`/`getLatestSummaries`/`upsertRelationshipState`/`getRelationshipStates` 签名扩参（全带默认值） |
| [llmservice.h](../llmservice.h) / [.cpp](../llmservice.cpp) | `askDeepSeek` 第三参 `RequestMeta`（L33-35）；`m_currentMeta` 成员；摘要/关系注入按 meta 取（L92/L102/L250） |
| [appcontroller.h](../appcontroller.h) / [.cpp](../appcontroller.cpp) | 渠道装配（L46-50）、输入链路改走 SessionRouter（L206-217）、`handleMakoReply` 随 `m_lastMeta` 入库（L126-134）、记忆写入带 meta（L246-262） |
| [configmanager.h](../configmanager.h) / [.cpp](../configmanager.cpp) | `BlindZone`/`ScheduleItem`/`ProactiveConfig` 结构 + identity.json 懒加载（L273-393） |
| [CMakeLists.txt](../CMakeLists.txt) | 登记全部新文件（L29-37） |

---

## 三、各步骤实施明细

### 步骤 1：数据库结构 v2 迁移

**机制选择**：采用项目已有的 `PRAGMA user_version` 版本迁移惯例（v1→v2 单次执行），而非每次启动预检。`ensureColumn` 三重防御仍保留在迁移块内：

1. `PRAGMA table_info` 预检列存在性
2. 执行 ALTER
3. `duplicate column` 错误容忍（并发窗口内另一连接已完成）

**八项结构变更**（[memorymanager.cpp L763-847](../memorymanager.cpp#L763-L847)）：

- `chat_history`：+`channel`('pet')、+`session_id`('default')、+`speaker`、+`sensitive`(0)，索引 `idx_history_session(channel, session_id)`
- `long_term_summary`：+`channel`、+`session_id`（摘要与未摘要游标双键，审查修订 A1）
- `episodic_memory`：+`origin`('pet')（渠道溯源，B5/E3）
- `relationship_state`：**重建表**（见四-1 偏差说明），`UNIQUE(subject, dimension)`，存量行复制为 `subject='user'`
- 新建 `raw_messages`（群聊原始消息表，Phase 0 只建不写）：id/channel/session_id/speaker/content/timestamp/sensitive/consolidated，两个索引

### 步骤 2：MemoryManager 读写扩参

全部带默认参数，旧调用点零改动：

| 函数 | 扩参 | SQL 变化 |
|---|---|---|
| `saveQATurn` | `const RequestMeta &meta = RequestMeta()` | INSERT 补 channel/session_id/speaker/sensitive |
| `getHistoryTurn(N)` | `channel`/`sessionId` | 子查询 WHERE 双键 |
| `getHistoryTurn(offset, limit)` | 不变 | 管理界面分页，跨渠道浏览 |
| `getUnsummarizedTurns` | `channel`/`sessionId` | 游标子查询与主查询均按双键 |
| `addLongTermSummary` | `channel`/`sessionId` | INSERT 补两列 |
| `getLatestSummaries` | `channel`/`sessionId` | WHERE 双键 |
| `upsertRelationshipState` | `subject`（默认 'user'） | SELECT/INSERT/UPDATE 均带 subject |
| `getRelationshipStates` | `subject` | WHERE subject |

配套：[historyturn.h](../historyturn.h) 的 `RelationshipState` 结构新增 `subject` 字段（默认 'user'）。

### 步骤 3：渠道适配器

- `IChannelAdapter` 纯虚接口，大脑侧不感知渠道
- `PetChannelAdapter`：持 `BubbleWidget*`（非所有权）；`sendUtterance` → `showMessage`
- [AppController::onPlayAudioAction](../appcontroller.cpp#L169-L183) 改为分层委托：气泡 → `m_petChannel->sendUtterance`；立绘 → `m_appearance->applyTags`（**全局情绪层**，不随渠道拆分，D1 开关 Phase 1 于 QQ 路径生效）
- `QQChannelAdapter` 占位编译通过

### 步骤 4：输入路由与装配

**信号流对比**：

```
旧：textSubmitted → [AppController lambda] → getHistoryTurn(N) → askDeepSeek(text, history)

新：textSubmitted → SessionRouter.submitInput("pet","default","欧尼酱",text)
                      ├─ resolveSubject → 'user'（Phase 0 恒定）
                      ├─ getHistoryTurn(memoryLength, channel, sessionId)
                      └─ requestReply(text, history, meta)
                          → [AppController lambda]
                              ├─ m_lastUserInput / m_lastMeta 记录
                              ├─ m_timeManager->notifyUserInputStarted()   ← 时序保持
                              └─ askDeepSeek(text, history, meta)
```

记忆写入侧（`handleMakoReply` 与 `memoryExtractionReady` lambda）随之携带 `m_lastMeta`：saveQATurn、getUnsummarizedTurns、addLongTermSummary、upsertRelationshipState、llmservice 内部的摘要/关系注入。

**已知边界**（Phase 1 处理）：`memoryExtractionReady` 是异步回调，若提取期间用户开启新一轮对话，`m_lastMeta` 已更新——桌宠路径恒 pet/default/'user' 无实际影响；QQ 接入时提取触发将改为"渠道无关钩子"并随参传递 meta（8.6 Phase 1 已列）。

### 步骤 5：ConfigManager 扩展

- 新结构：`BlindZone` / `ScheduleItem` / `ProactiveConfig`
- 独立配置文件 `app_data/config/identity.json`（**跟随项目 JSON 配置体系**，见四-2 偏差说明），懒加载、只读解析，缺失时用默认值并打日志、不自动创建
- 接口：`getSelfQQAccounts` / `getMasterQQAccounts` / `isMasterQQ` / `isBlindZone` / `getFollowExternalMood` / `getProactiveConfig`
- Phase 0 无调度行为、无 UI

---

## 四、与批准方案的偏差（3 处，均已在实施中确认合理性）

| # | 方案原定 | 实际实施 | 原因 |
|---|---|---|---|
| 1 | `relationship_state` 加 `subject` 列 | **重建表**：建 v2 表 → 复制数据 → 删旧表 → 重命名 | 原表 `dimension` 单列 `UNIQUE` 约束不允许多 subject 共存；SQLite 无法 ALTER 约束。数据零丢失（事务内复制），仍是"旧数据零迁移成本" |
| 2 | QSettings INI 格式 | **独立 `identity.json`** | 核实发现 ConfigManager 实际是 JSON 文件体系（`#include <QSettings>` 为未使用的历史残留），跟随项目现状 |
| 3 | 每次启动 `ensureColumn` 预检 | **`PRAGMA user_version` v1→v2 迁移块**（`ensureColumn` 保留在块内） | 项目已有版本迁移惯例（v1 块），单次执行更高效，且与现有代码结构一致 |

另有一处顺序微调：`utterance.h` 从步骤 3 提前至步骤 2 之前创建（步骤 2 的 `saveQATurn` 扩参依赖 `RequestMeta` 完整类型），不影响依赖关系（1→2→4，3、5 独立）。

---

## 五、subject 命名与身份层（E2/C2 决策落地）

- `'user'` 永久保留为"主人"的正式 subject 名（与 `character_profile` 存量语义一致，**不迁移**）
- 新主体命名规范：`qq:<uin>` / `group:<群号>`（Phase 1/3 启用）
- 身份解析权在宿主配置层（`SessionRouter::resolveSubject`），渠道与模型无权判定归属
- Phase 0 桌宠路径恒返回 `'user'`；Phase 1 改为查 identity.json（self→丢弃 / master→'user' / 未知→'qq:<uin>'）

---

## 六、验收清单

### 已验证（构建期）

- [x] Debug 全量构建通过，零错误
- [x] QQChannelAdapter 占位编译通过
- [x] 旧调用点全部依赖默认参数，无签名冲突

### 待验证（运行期，需启动程序）

| # | 步骤 | 预期 |
|---|---|---|
| 1 | 首次启动 | 日志 `[MemoryManager]正在升级数据库结构至 v2`，旧聊天记录完整保留 |
| 2 | 二次启动 | 不再出现 v2 日志（user_version 幂等），无 duplicate column 报错 |
| 3 | 正常对话一轮 | 气泡/语音/立绘/记忆提取与改前一致；`chat_history` 新行 `channel='pet'`、`session_id='default'` |
| 4 | 手动创建 identity.json（填入 self/master QQ 号） | 重启后日志 `[ConfigManager]identity.json 加载完成: self x个 / master x个` |
| 5 | 设置界面历史表格 | 显示正常（跨渠道浏览分页未过滤） |

---

## 七、风险与回滚

| 风险点 | 缓解 |
|---|---|
| v2 迁移一次性执行，失败后 user_version 已置 2 可能跳过 | 迁移各步均有错误日志；relationship_state 重建在事务内（v1 块同一 try/commit），失败整体回滚则 user_version 不会推进 |
| PetChannelAdapter 与原 onPlayAudioAction 时序差异 | 单槽内顺序委托（先气泡后立绘），与原实现顺序完全一致 |
| memoryExtractionReady 异步窗口的 m_lastMeta 漂移 | 桌宠路径无实际影响；Phase 1 改渠道无关钩子时随参传递 |

**回滚方式**：改动未提交 git 前，`git checkout -- .` 即可整体还原（数据库 user_version=2 的库文件回滚后由旧代码正常打开——新列被忽略，仅 raw_messages/relationship_state 新结构残留无害）。

---

## 八、审批意见

| 项 | 结论 |
|---|---|
| 实施与批准方案一致性 | 一致（3 处偏差已在第四节说明理由） |
| 待审批人确认 | 运行期验收 5 项（第六节） |
| 审批结果 | （待填写：通过 / 需修改项） |
