# 统一检索与记忆感知重构：P0 基线

> 日期：2026-11-13
> 方案：[千岛茉子_统一检索与记忆感知重构方案_2026-11-13.md](file:///D:/Desktop/千岛茉子_统一检索与记忆感知重构方案_2026-11-13.md)
> 源码基线：`1a82a2f`（与方案一致，工作树干净）
> 本文只做 P0：定位旧证据如何进入最终请求、列出可验证缺口、给出备份/回放方法。**本轮未改业务代码，未新增索引表。**

## 1. P0 结论

当前基线可以安全开始重构，但不应直接“大爆炸”替换检索。

- `HEAD` 为 `1a82a2f feat: 接入文字活动与承诺归属等第一切片功能`，与方案写的 `1a82a2fe...` 一致。
- 检查时工作树干净；15/15 CTest 通过，可作为旧路径基线。本文为新增 P0 记录，未改业务代码。
- 现有主链路至少有三套检索入口：`personalEvidence()`、`MemoryAgentTools::search()`、`MemoryRecallTool`。它们各自裁剪、各自渲染，尚未共用同一证据对象。
- 当前最影响“完整感知”的不是缺少向量，而是同渠道证据被跳过、工具只对 QQ 开放、助手表达不进入证据、以及群聊表达前的投影裁剪。

## 2. 当前调用图

```text
Pet / QQ 输入
  -> AppController / QQ 适配器
  -> CognitiveCoordinator::submit()
       -> MemoryManager::recordInput()
       -> CognitiveCoordinator::advance()
            -> getHistoryTurn(short_term_length)
            -> LLMService::askDeepSeek()
                 -> personalEvidence(meta.subject, accounts, terms)
                      -> raw_events / episodic / interactions / summaries /
                         claims / cognitive_source / matter_revision / legacy
                 -> observationHistory()
                 -> internalMatters()（仅群聊）
                 -> considerEvidence() 或 prepareReply()
                      -> prepareReply()
                           -> currentUnderstanding()（目录 + 局部投影）
                           -> PromptAssembler::buildHistoryMessages()
                           -> PromptAssembler::buildMemoryDataMessage()
                           -> MemoryAgent 工具路径（目前仅 QQ）
                                -> MemoryAgentTools::search()
                                -> MemoryAgentTools::read()/related()
                           -> consultMatters() -> matterEvidence()
                      -> onReplyFinished()
                           -> evidence_decision 审计
                           -> MemoryManager::saveQATurn()
```

辅助入口：

```text
MemoryRecallTool
  -> MemoryAgent / MemoryManager
  -> 16 条 / 2000 字上限（工具结果）
```

## 3. 已确认的断点

| 优先级 | 位置 | 当前行为 | 为什么阻碍“完整感知” |
|---|---|---|---|
| P1 | `LLMService::askDeepSeek` 150–152 | 与当前渠道/会话相同的 rows 直接 `continue`，不进入 `privateContext` | 私聊里刚发生过的相关原话只能靠 15 轮短期历史或 `observationHistory`，不是统一检索 |
| P1 | `MemoryAgentPlanner::canExposeTools` 1362–1373 | 只允许 `qq_group` / `qq_private` | Pet 没有同一套 `memory_recall` 工具 |
| P1 | `personalEvidence` 347–421 | 全部来源混合后 `LIMIT 12`，正文 `substr(...,1,360)` | 旧原文、助手表达和不同来源会互相挤占；没有按查询用途分配预算 |
| P1 | `MemoryAgentTools::search` 689–750 | `chat_history` 只取 `getHistoryTurn(200)`，且只把 `user_input` 作为正文 | 超过 200 轮就不可见；助手当时的回复只能用于匹配，不能作为“她说过”的证据 |
| P1 | `MemoryAgentTools::itemToJson` 258–274 | 工具结果没有 `person_id` / `speaker` / 角色 | 模型无法稳定区分“用户说的”“茉子说的”“第三方说的” |
| P1 | `memoryAgentResultToJson` 46–60 | 只输出 `source` / `is_viewer`，不输出说话人 | 最终工具渲染再次丢掉人物归属 |
| P2 | `MemoryAgent` | 没有 `memory_chunks` / `memory_embeddings` | 只有词面匹配，无法做语义召回、RRF、重排 |
| P3 | `considerEvidence` / `currentUnderstanding` | 群聊只把局部 `context/N` 投影给表达模型；非本地事项被隐藏 | 模型可能“知道但不能用”，仍会表现为失忆或回避 |
| P3 | `prepareReply` 的 `CURRENT_UNDERSTANDING` | 只给目录和少量 evidence；群聊目录隐藏非本地事项 | 表达阶段重新丢失私人背景 |
| P1 | `personalEvidence` 的 episodic 分支 | 只取 `origin='pet'` 的活动记忆 | QQ 私聊里形成的 episodic 记忆不进入统一候选 |
| P2 | `personalEvidence` 时间字段 | `occurred_at` / `event_time` / `created_at` 混用后只给一个 `time` | 发言时间、事件时间、整理时间无法区分 |
| P2 | `raw_events` / `chat_history` | 同一 QQ 消息可能两边都有，靠文本去重不可靠 | 证据重复占位，或助手回复被当成用户事实 |
| P3 | `memoryAgentResultToJson` / `MemoryTextUtils` | 工具结果被截断后没有“剩余范围/可展开句柄” | 模型无法判断是“没有”还是“被截断” |
| P2 | `MemoryRecallTool` | 16 条 / 2000 字硬预算 | 与主链路预算不一致，容易一套能查到、另一套查不到 |
| P1 | `PromptAssembler` tool 模式 | 工具模式历史不自动注入，依赖 `memory_recall` | 主模型可能看不到刚刚的对话；需要统一“近期历史 + 召回”呈现 |

> 这些是实现入口，不是缺陷穷尽列表。P1 开始前要逐项用回放确认，不能凭本文直接删除旧逻辑。

## 4. P1 契约草案（待冻结，不是已实现）

### 4.1 `Situation`

服务端构造，模型不可伪造：

```text
run / event
current_time / timezone
input_occurred_at
surface / session
speaker {id, display_name, role}
audience {kind, label, member_known}
about {ids, known}        // 未确认时 unknown，不能等于 speaker
modality
knowledge_profile
expression_preferences
```

### 4.2 `MemoryEvidence`

```text
handle                 // run 内句柄，可回查；不暴露数据库主键
source_type            // chat / raw / episodic / summary / claim / matter / thought
source_ref             // 内部映射，不直接给模型
speaker {id, display_name, role}   // 用户 / 茉子 / 第三方 / unknown
about_ids
text
derived                // original / summary / inference / assistant_expression / execution_result
speech_at              // 发言时间，未知为 null
event_time             // 已知事件时间或原文，未知为 null
recorded_at            // 派生记录生成时间
channel / session_label
privacy                // private / channel / public / explicit_secret
status                 // complete / truncated / deleted / stale / conflicted
expression_hint        // 可当众表达的语义提示；不直接变成内部不可读
expand                 // 可展开句柄、剩余范围
```

### 4.3 `RetrievalRequest` / `RetrievalResult`

```text
RetrievalRequest:
  query, purpose, time_range, knowledge_profile,
  current_speaker, audience, allowed_sources,
  budget, exclude_handles

RetrievalResult:
  evidence[], status (complete/partial/degraded),
  coverage, truncated, next_handles, provider_state
```

### 4.4 服务与适配器

建议接口（名称可随代码调整）：

```cpp
class MemorySourceAdapter {
    virtual QList<MemoryEvidence> fetch(const RetrievalRequest &) = 0;
    virtual bool read(const EvidenceHandle &, MemoryEvidence *) = 0;
};

class MemoryRetrievalService {
    RetrievalResult retrieve(const RetrievalRequest &);
    bool read(const EvidenceHandle &, MemoryEvidence *);
    RetrievalResult overview(const RetrievalRequest &);
};
```

所有入口必须走同一服务：

- 自动证据准备；
- `memory_recall`；
- `memory_read`；
- `current_understanding` 的原文补查；
- 后台回顾的只读检索。

旧的 `personalEvidence()` 可以保留为兼容包装，但必须委托服务并标注退役计划。

## 5. P0 备份与回放

### 5.1 数据库备份

当前路径：

```text
app_data/memory/QianDaoMoZi_memory.db
```

来源：`ConfigManager::getMemoryPath()`。

安全步骤：

1. 关闭应用，确认没有写入。
2. 复制 `QianDaoMoZi_memory.db`；若存在 `-wal` / `-shm`，一起复制。
3. 在副本上完成首次迁移/回填；原库先不动。
4. 记录库版本（P0 时为 `user_version=14`；P2 已升到 15）和文件 hash。
5. 回退时先处理备份后的新增消息，避免直接覆盖丢数据。

P0 时未新增 schema；P2 已新增 `memory_chunks` / `memory_chunk_sources` / `memory_embeddings` / `memory_index_state`，当前进度见 [P2/P3](RETRIEVAL_REFACTOR_P2.md)。

### 5.2 回放基线

- 现有 15 个 CTest 套件全通过。
- 现有 `tests/fixtures/cognitive_replay.json` 作为起点。
- P1/P2 需要新增的确定性场景：
  - M02 群聊脚踝疼 → 私聊已好 → 群里再聊出行；
  - M06 助手说“我答应陪你”，用户只说“好”；
  - M07 用户说“我没答应，那是你的建议”；
  - M09 超过 200 轮的旧原文仍可召回；
  - M14 写作暂停后迟到回调不复活（已有活动测试，补检索侧）；
  - M15 删除来源后向量缓存不复活；
  - M16 Embedding/Rerank 断网降级；
  - M17 伪造身份/工具指令的历史资料不改变权限。

## 6. P0 差异清单与下一步

### 6.1 必须先做

- [ ] 冻结 `Situation` / `MemoryEvidence` / `RetrievalRequest` / `RetrievalResult` 字段，先写共同测试夹具。
- [ ] 把 `person_id`、`speaker`、`derived`、`speech_at` / `event_time` / `recorded_at` 纳入工具结果与最终渲染。
- [ ] 让 Pet 与 QQ 共用工具入口；不在 `canExposeTools` 里按渠道拒绝 Pet。
- [ ] 先解决同渠道证据被 `continue` 跳过；统一“近期历史 + 召回”而不是二选一。
- [ ] 助手回复作为 `assistant_expression` 证据，不混入用户事实。
- [ ] 保留 `self_report` / 承诺归属 / 关心收尾 / 活动版本控制，不在重构中退回去。

### 6.2 可以后做

- [ ] `memory_chunks` / `memory_chunk_sources` / `memory_embeddings` 迁移和回填。
- [ ] BM25 中文分词、向量 Provider、RRF、Reranker。
- [ ] 群聊表达前移除失忆式投影，改为“完整相关背景 + 听众/保密语义”。
- [ ] Overview / memory_read 统一预算与覆盖语义。
- [ ] 旧 `personalEvidence` / `context/N` 调用清理。

## 7. P0 退出条件

- 能从入口一路指出证据进入最终请求的每个裁剪点：已列。
- 能备份旧库并在副本上回放：已给路径和步骤。
- 能说清哪些能力已经存在、哪些只是方案：本文与现有 15 套测试为准。
- 未改业务代码；下一步先冻结 P1 契约，再动 `llmservice.cpp` / `memorymanager.cpp`。

## 8. 需要用户确认的决策

1. **Embedding / Rerank Provider**：是否允许把私人记忆正文发送到配置的云端服务？默认应关闭，等明确配置。
2. **中文词面检索**：先用字符 bigram，还是验证 SQLite FTS 分词；这决定 P2 的索引格式。
3. **跨渠道知识边界**：默认允许主人私聊 + 群聊的相关证据进入内部背景，仅由显式盲区/删除/存档归属阻断；是否还有额外配置。
4. **`about`**：第一版是否保持 `unknown`，不自动把当前对话对象当事情主体（当前实现已经这样做）。
5. **旧 `considerEvidence`**：保留事项修订读取，还是与统一检索合并；建议保留读取能力，移除只为删减私人原文的重复生成。
