# 统一检索重构：P1 第一切片

> 日期：2026-11-13
> 基线：`1a82a2f`，P0 见 [RETRIEVAL_REFACTOR_P0.md](RETRIEVAL_REFACTOR_P0.md)
> 范围：共同类型、统一自动证据入口、Situation 复用。**未接向量，未改工具检索。**

## 已实现

- 新增 [memoryevidence.h](memoryevidence.h) / [memoryevidence.cpp](memoryevidence.cpp)：
  - `MemorySituation`：run/event、当前时间、输入发言时间、surface、speaker、audience、about、modality、privacy、knowledge_profile、expression_preferences。
  - `MemoryEvidence`：handle、来源类型/来源映射、说话人、正文、派生性质、发言/事件/记录时间、渠道、隐私、状态。
  - `RetrievalRequest` / `RetrievalResult`。
- 新增 [memoryretrieval.h](memoryretrieval.h) / [memoryretrieval.cpp](memoryretrieval.cpp)：
  - `MemorySourceAdapter` 与 `LegacyMemorySourceAdapter`。
  - `MemoryRetrievalService::retrieve()` 返回共同证据；失败时 `degraded`。
- `LLMService::askDeepSeek` 的自动证据准备改为调用统一服务；Pet 与 QQ 私聊/群聊走同一入口。
- `personalEvidence()` 增加 `excludeCurrent` 参数（默认 `true`，保持旧调用）；统一服务传 `false`，同渠道历史不再被无条件跳过。
- `Situation` 在 `askDeepSeek` 构建一次，`considerEvidence` 与 `prepareReply` 复用，避免前后场景不一致。
- `PromptAssembler::buildSituationBlock` 改为使用同一 `MemorySituation`，不再维护第二份拼接逻辑。
- 自动证据仍保留显式盲区过滤、敏感内容过滤和当前消息去重。
- 新增 `tst_memorymanager` 用例：同渠道证据可检索、说话人字段正确、`excludeCurrent=true` 可回到旧行为、Situation 的 `about=unknown`。

## 验证

- 15/15 CTest 通过。
- 未新增 schema；P2 已把库升到 v15。
- 未调用真实 Embedding/Reranker；本轮不需要云模型即可回放。

## 还没做

- `MemoryAgentTools::search` 仍使用自己的逐源查询、200 轮上限；未整体委托统一服务。
- 工具结果已补 `person_id` / `speaker` / `speaker_role` / `derived`，但时间语义仍是单一 `time`。
- `considerEvidence` / `context/N` 的群聊表达投影已部分移除：表达模型获得 background + personal；兼容投影仍保留。
- `memory_chunks` / `memory_embeddings` 已在 P2 接入；真实 Embedding/Reranker 仍未验证。
- 同渠道证据已进入候选，但尚未与近期 15 轮历史做完整去重。

## 下一步

1. `MemoryAgentTools::search` 改为调用 `MemoryRetrievalService`，或让两者共用同一证据查询。
2. 把 `speech_at` / `event_time` / `recorded_at` 独立渲染，不再只给一个 `time`。
3. 接入真实 Embedding / Reranker Provider，并做小样验证。
4. 自动/异步索引回填、索引代次切换与删除回归。
5. 真实 QQ/Pet 渠道验收，记录泄漏与失忆样例。
