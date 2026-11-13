# 当前状态与阅读入口

> 更新：2026-11-12 · 基于 `6a6a73d` 的工作树
> 本文只回答“现在在哪”。能力清单看 [开发路线图](ROADMAP.md)，实现细节看 [架构与当前实现](ARCHITECTURE.md)，历史看 [开发历史](HISTORY.md)。

## 一句话

**M1 地基已完成；M2 收尾中；M3 第一切片已接入，待真实使用验收；M4 以后再做。**

## M1–M4

| 里程碑 | 含义 | 状态 | 对应编号 |
|---|---|---|---|
| **M1 地基** | 记忆、证据、渠道、语音、审计能跑通 | 已完成 | 任务 1–3；PR-4/6/7/8/9；v15 |
| **M2 跨渠道表达** | 同一个她在 pet / QQ 私聊 / 群聊说得对、说得合适 | 进行中（承诺归属/关心收尾已接入；A5 待真实群复验） | 阶段 A；任务 4–5；A1–A5 |
| **M3 生活引擎** | 她有自己的活动、回顾、主动交流 | 第一切片已接入，待真实使用验收 | 阶段 B；任务 6；B1–B5 |
| **M4 外部世界/成长** | 浏览、绘画、学习，人格/情绪/关系长期成长 | 以后 | 阶段 C（未定义） |

## 接下来验收三件事

1. **A5 真实渠道复验**：同一句承诺在 pet、QQ 私聊、群聊各跑一次；确认承诺归属、关心收尾、群聊只确认所指且不公开无关背景。
2. **私聊归属/收尾实机**：确认“你答应过”只引用用户原话；用户说“已经躺下”后不再新增义务。
3. **活动真实使用**：写一段草稿 → 重启读回 → 继续写 → 写作中暂停 → 迟到回调不复活。

> 统一检索与记忆感知重构另开阶段：总进度见 [RETRIEVAL_REFACTOR_STATUS.md](RETRIEVAL_REFACTOR_STATUS.md)，P0/P1/P2 细节见同名文档。已接入统一自动证据、Pet/QQ 工具入口、助手证据、v15 检索块、词面/RRF 与可选向量；真实 Embedding/Reranker、自动异步回填和真实渠道仍未验收。

## 已完成 / 进行中 / 未开始

**已完成（工程路径 + 自动测试）**

- 任务 1–3：回放定位、事项/证据契约、认识更新可靠提交。
- PR-4、PR-6/7/8/9：legacy MemoryAgent、Memory Recall Tool、Overview、TimeFrame。
- v15：`cognitive_source` / `matter_revision` / `observation_history` / `cognitive_audit` 证据链，以及可重建的 `memory_chunks` / `memory_chunk_sources` / `memory_embeddings` / `memory_index_state`。
- 统一检索：`MemoryEvidence` / `MemoryRetrievalService`；Pet/QQ 自动证据同入口；同渠道历史进入候选；工具补助手表达、说话人字段并开放 Pet。
- 混合召回：v15 检索块、词面 BM25、可选向量、RRF、启发式重排与旧适配器回退；`--rebuild-memory-index` / `--memory-index-status` / `--clear-memory-index` / `--backup-memory-db`；新消息增量写 chunk；`auto_backfill` 使用独立线程与独立 SQL 连接；审计记录 backend/候选数/辅助来源/延迟；DashScope Embedding 已接入后台索引，查询侧通过独立 `MemoryRetrievalWorker`；凭证可存 `setting.json` 的 `api.dashscope_api_key`。
- 工具对话检索不再固定只取最近 200 轮；`recent`、`chat_history`、`interaction_summary`、`long_term_summary`、`claim`、`episodic` 优先走统一服务，索引未就绪时按配置回退；助手表达作为独立证据返回；`speaker_id` 与本轮 viewer 比较，owner 群可按配置看到跨渠道私聊。
- 连续感知：`considerEvidence` 把 background + personal 交给表达模型；`currentUnderstanding` 支持 audience-private；群聊不再只靠失忆式投影。
- 群聊跨渠道表达：`context/N` 最小片段投影、`excerpts` 校验、空决定拦截（`6a6a73d`）。
- 承诺归属：`CURRENT_UNDERSTANDING` 增加 `self_report` 原话；证据带 `speaker_role/source_kind`；`attribution_check` 分编号、说话人、语义三层审计。
- 关心收尾：`care` 数组按具体事项绑定；不把“上床”扩展成“已入睡/已洗澡/作息已调整”。
- `Situation`：surface / speaker / audience / about / modality / privacy 进入 prompt。
- 文字活动：`thought_result` 增加 progress/next_step/paused；`applyThoughtAction` 带 ID/版本；迟到回调与分享有 guard。
- 开关拆分：`proactive.private_writing` 控制写作/回顾，`proactive.enabled` 控制主动联系/分享。
- 15 个 CTest 套件通过；新增用例覆盖 self_report、归属审计、Situation、活动暂停/恢复/迟到回调、v14→v15 重开与索引回填。

**进行中（真实使用验收）**

- 阶段 A 的 A5：真实群聊复验，以及私聊“承诺 vs 建议”实机。
- 阶段 B 第一切片：真实写一段草稿、重启读回、继续/暂停；不是完整 B1/B3/B4。

**未开始**

- B1 统一事件与注意力（本切片未触碰）。
- B3 的自主形成新目标（本切片只做回顾/续写）。
- B4 的送达回执与投递确认（本切片只记分享尝试）。
- 阶段 C：外部世界、人格成长、情绪模型、多角色/多存档。
- PR-5 留存 UI：旧账，建议并入管理员“今天”面板，不单开任务。

## 旧编号怎么处理

- **PR-3.2b / 3.2c**（claim 三主体、跨群 viewer 过滤）：并进 A5 的真实群验收。
- **PR-5**（留存 UI）：并进 B2/B5 的管理员面板。
- **其他 PR**：已完成或已退役，只留在历史文档，不再作为主线。

## 编号说明

| 编号 | 例子 | 含义 | 现在怎么用 |
|---|---|---|---|
| **M1–M4** | M2 | 里程碑，给人看的 | 主入口 |
| **任务 1–6** | 任务 4 | 工程能力清单 | 只在 ROADMAP |
| **阶段 A/B/C** | 阶段 B | 交付批次 | 只在 ROADMAP |
| **A1–A5 / B1–B5** | A5 | 阶段子项 | 只在 ROADMAP |
| **PR-n** | PR-5 | 旧 R3 施工单 | 只作历史，不再新增 |
| **v15** | v15 | 数据库结构版本 | 只在 ARCHITECTURE / 配置 |

## 文档地图

| 想知道 | 读 |
|---|---|
| 现在在哪、下一步做什么 | 本文 |
| 能力清单、阶段 A/B、B1–B5 | [开发路线图](ROADMAP.md) |
| 当前组件、数据流、v15、限制 | [架构与当前实现](ARCHITECTURE.md) |
| 配置字段与默认值 | [配置参考](CONFIG_REFERENCE.md) |
| `memory_recall` / `memory_read` 契约 | [记忆检索接口](MEMORY_RECALL_TOOL.md) |
| R3 旧基线、PR 编号来源 | [R3 历史基线](MEMORY_V5_R3_BASELINE.md) |
| 语音与 GPT-SoVITS | [语音参考](GPT_SOVITS_REFERENCE.md) |
| 历史阶段与文档合并去向 | [开发历史](HISTORY.md) |
| 安装、构建、操作 | [项目 README](../README.md) |

## 维护约定

- 状态变化先改本文，再改 ROADMAP / ARCHITECTURE；三者不要各写一份“当前状态”。
- 新工作不再使用 PR 编号；需要追溯旧 R3 工作时才引用。
- 每个阶段完成时记录日期、版本、验证层级（自动测试 / 真实接口 / 实际渠道）。
- 历史文档只追加，不重写结论；过期内容加“历史”横幅。
