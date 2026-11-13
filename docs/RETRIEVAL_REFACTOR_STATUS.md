# 统一检索重构：P0–P5 状态

> 日期：2026-11-13
> 方案：`D:\Desktop\千岛茉子_统一检索与记忆感知重构方案_2026-11-13.md`
> 基线：`1a82a2f`

## 总览

| 阶段 | 状态 | 已完成 | 未完成 |
|---|---|---|---|
| P0 基线 | 完成 | 调用图、缺口、备份/回放、契约草案 | — |
| P1 契约/入口 | 大部分完成 | `MemorySituation`/`MemoryEvidence`/`RetrievalRequest`/`Result`；Pet/QQ 自动证据同服务；工具助手证据、说话人字段；Pet 工具入口 | `MemoryAgentTools::search` 仍逐源查询；时间只给一个 `time` |
| P2 检索 | 部分完成 | v15 四张索引表；`--rebuild-memory-index` / `--memory-index-status`；增量写入；词面 BM25；可选向量；RRF；启发式重排；旧适配器回退；删除触发器 | 真实 Embedding/Reranker Provider；断点续跑；生产级中文 FTS；规模性能；索引代次切换 |
| P3 感知 | 部分完成 | `considerEvidence` 把 background + personal 给表达模型；`currentUnderstanding` audience-private；同渠道历史进入候选 | 真实模型语义验收；输出泄露实测；完整去重 |
| P4 迁移/验收 | 部分完成 | v15 迁移、命令重建、后台 Worker、增量写入、备份文档、删除失效测试 | 真实库副本回填；断点续跑；真实 QQ/Pet 渠道；性能/费用 |
| P5 收尾 | 部分完成 | 文档、状态、旧路径标注 | 旧 `personalEvidence`/`context/N` 清理；默认路径切换；最终验收报告 |

## 关键事实

- 15/15 CTest 通过。
- 数据库 `user_version=15`。
- `LocalHashEmbeddingProvider` 只用于测试/降级，不是语义模型；真实 Provider 尚未接入。
- 自动证据服务已统一；工具检索仍是第二套实现，不能宣称“全部入口已统一”。
- 群聊表达模型现在能看到相关私人背景，但“不泄露”仍依赖模型判断和现有输出检查，没有零泄露保证。
- 没有真实渠道、真实 Embedding、Reranker 或长库性能数据。

## 下一步

1. 把 `MemoryAgentTools::search` 的 chat/raw 部分委托 `MemoryRetrievalService`，保留 overview/need 的独立语义。
2. 接入真实 Embedding/Reranker，配置化模型、维度、超时与失败降级；做小样 Recall@10 对照。
3. 实现异步索引任务、断点续跑、索引代次切换；启动时只在显式配置下回填。
4. 用真实库副本跑 P4：备份、迁移、重建、删除、重启、回退。
5. 真实 QQ/Pet 渠道验证 M01–M18，记录通过/失败样例，不删除失败案例。
