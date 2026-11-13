# 统一检索重构：P2/P3 进度

> 日期：2026-11-13
> 基线：`1a82a2f`
> 前置：[P0](RETRIEVAL_REFACTOR_P0.md)、[P1](RETRIEVAL_REFACTOR_P1.md)

## P1.2 工具侧

- `chat_history` 工具分支现在同时返回：
  - 用户原话：`speaker_role=user`、`derived=original`；
  - 茉子当时的回复：`speaker_role=assistant`、`derived=assistant_expression`。
- `MemoryAgentItem` / `itemToJson` / `memoryAgentResultToJson` 输出 `person_id`、`speaker`、`speaker_role`、`derived`。
- `MemoryAgentPlanner::canExposeTools` 允许 Pet，不再只限 QQ；工具仍只对 `reply` 开放。
- `MemoryAgentTools::search` 的 `chat_history` 优先调用 `MemoryRetrievalService`（chunk 证据），索引未命中/未就绪时回退 `searchHistoryTurns`，不再固定只取最近 200 轮。
- `MemoryAgentTools::search` 的其余来源仍逐源查询，尚未整体委托 `MemoryRetrievalService`；这一点留到 P5 清理。

## P2 索引与混合召回

### 已实现

- schema v15，新增：
  - `memory_chunks`：正文、来源、说话人、时间、隐私、状态、索引代次、内容 hash。
  - `memory_chunk_sources`：chunk 到原始表/ID 的映射。
  - `memory_embeddings`：chunk、模型配置、维度、float32 BLOB、内容 hash、索引代次。
  - `memory_index_state`。
- 删除触发器：删除 `raw_events` / `chat_history` 时使对应 chunk 失效；删除 chunk 时清理来源映射与向量。
- `MemoryManager::rebuildSearchIndex()`：从 `raw_events` 与 `chat_history` 重建检索块；可重复执行，不修改原表。
- 新增/更新 `raw_events` 与 `chat_history` 时同步增量写 chunk；删除来源由触发器失效。整库重建仍用于旧库回填。
- 命令行重建：
  ```text
  Windows_AI.exe --rebuild-memory-index
  ```
- 命令行状态：
   ```text
   Windows_AI.exe --memory-index-status
   ```
   返回 chunk/embedding 数量、按来源分桶与 `memory_index_state` 的 `status/last_rebuild/generation/duration_ms`。
- 命令行清空索引（不删除原文，可重建）：
   ```text
   Windows_AI.exe --clear-memory-index
   ```
- 命令行一致性备份（`VACUUM INTO`，生成带时间戳副本）：
   ```text
   Windows_AI.exe --backup-memory-db
   ```
- 可选 `memory.retrieval.auto_backfill=true`：启动时若索引为空，在独立 `QThread` + 独立 SQL 连接中重建；不阻塞 UI。默认关闭。
- `MemoryRetrievalService`：
  - 有索引时先走向量/词面候选，再用兼容适配器补 summary / claim / episodic，避免索引存在后这些来源消失。
  - 词面：中文按查询词项做 BM25 评分。
  - 向量：可选 `MemoryEmbeddingProvider`；查询向量与 `memory_embeddings` 做 cosine。
  - RRF 融合；默认 `HeuristicMemoryReranker` 按 RRF 顺序 + 词面命中 + 时间近因重排，**不是语义重排**。
  - 索引失败或为空时回退 `LegacyMemorySourceAdapter`；Query embedding 失败时保留词面结果并返回 `degraded`（有自动测试）。
- `RetrievalRequest` 支持 `sourceKinds` 过滤和 `allowLegacyFallback`；关闭 legacy fallback 后只走索引，便于 P5 切换默认路径。测试覆盖 raw-only 与无回退返回空。
- `RetrievalResult` 记录 backend / 候选数 / 词面命中数 / 向量命中数 / 延迟；`answer_decision` 审计带这些字段。
- 新增 `LocalHashEmbeddingProvider`：只做字符 bigram 哈希，明确标为 `local_hash_test`，用于测试 RRF/降级路径，不能当成语义模型。`embedding_enabled=true` + `embedding_provider=local_hash_test` 时会写入/读取测试向量，仅用于验证路径。

### 未完成

- 没有真实 Embedding / Reranker Provider 配置和网络验证；`memory.retrieval` 已解析 `candidate_limit` / `rerank_limit` / `embedding_*`，但 `embedding_provider` 目前只接受 `local_hash_test`，真实模型未接入。
- 后台 Worker 已支持 `auto_backfill` 的独立线程重建；但没有断点续跑、任务队列、索引代次切换。索引为空时回退旧适配器。
- 没有断点续跑或索引代次切换；后台 Worker 已用独立连接，但仍是整库重建，不是增量任务队列。
- 词面仍是 C++ 子串计数，不是生产级中文 BM25/FTS；chunk 查询使用 `LIKE` 限制候选，规模大时需换 FTS 或倒排索引。
- 查询向量目前只在测试 Provider 下验证；真实模型 Recall@10 尚未测。

## P3 连续感知

- `considerEvidence` 调用 `prepareReply` 时传入 `background + personal`，表达模型保留本轮相关背景，不再只接收删减后的 `context/N`。
- `currentUnderstanding(..., includeAudiencePrivate=true)` 供 `prepareReply` 使用；群聊表达可以看到相关私人事项与 `self_report` 原话，仍由 Situation/guidance 约束听众和保密。
- 旧的 `avoid_*` 投影路径仍在 `considerEvidence` / 兼容调用中保留；表达路径不再靠它失忆。
- 测试更新为“私人背景可以进入群聊最终 prompt，但模型输出不得泄露”；固定模型输出仍为公开短句。
- 未做真实模型语义验收；输出泄露仍由模型判断 + 现有输出检查，不是零泄露保证。

## 验证

- 15/15 CTest 通过。
- 新增：v15 迁移、v14 回退后重开、索引重建、同渠道证据、BM25/向量/RRF 服务、删除来源使 chunk 失效、`currentUnderstanding` 的 audience-private 开关。
- 未验证：真实 QQ/Pet 渠道、真实 Embedding、真实 Reranker、长库性能、重启续跑。
