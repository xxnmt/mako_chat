# 文档索引

> 更新：2026-11-13。**先读 [当前状态](STATUS.md)**；本文只负责“去哪找”。

## 阅读顺序

1. [当前状态](STATUS.md) — 现在在哪、下一步做什么。
2. [开发路线图](ROADMAP.md) — 能力清单、阶段 A/B、B1–B5、验收。
3. [架构与当前实现](ARCHITECTURE.md) — 当前组件、数据流、v15、已知限制。
4. [配置参考](CONFIG_REFERENCE.md) / [记忆检索接口](MEMORY_RECALL_TOOL.md) / [语音参考](GPT_SOVITS_REFERENCE.md) — 按需查。
5. [统一检索进度](RETRIEVAL_REFACTOR_STATUS.md) / [P0](RETRIEVAL_REFACTOR_P0.md) / [P1](RETRIEVAL_REFACTOR_P1.md) / [P2/P3](RETRIEVAL_REFACTOR_P2.md) — 检索重构的基线、缺口与进度。
6. [开发历史](HISTORY.md) / [R3 历史基线](MEMORY_V5_R3_BASELINE.md) — 只用于追溯，不要当当前实现。

## 文档地图

| 要了解的内容 | 文档 |
|---|---|
| 现在在哪、里程碑 M1–M4、下一步 | [当前状态](STATUS.md) |
| 能力清单、阶段 A/B/C、B1–B5、验收 | [开发路线图](ROADMAP.md) |
| 组件职责、正常对话路径、记忆与持久化、v15 | [架构与当前实现](ARCHITECTURE.md) |
| 统一检索重构的 P0/P1/P2 进度 | [总览](RETRIEVAL_REFACTOR_STATUS.md) / [P0](RETRIEVAL_REFACTOR_P0.md) / [P1](RETRIEVAL_REFACTOR_P1.md) / [P2/P3](RETRIEVAL_REFACTOR_P2.md) |
| `setting.json`、`identity.json` 与默认值 | [配置参考](CONFIG_REFERENCE.md) |
| `memory_recall` / `memory_read`、时间框架、概览 | [记忆检索接口](MEMORY_RECALL_TOOL.md) |
| R3 旧基线、PR 编号来源 | [R3 历史基线](MEMORY_V5_R3_BASELINE.md) |
| GPT-SoVITS HTTP 与备选调用 | [语音参考](GPT_SOVITS_REFERENCE.md) |
| 历史阶段、决策、文档合并去向 | [开发历史](HISTORY.md) |
| 安装、构建、操作、常见问题 | [项目 README](../README.md) |

## 编号约定

这些编号是不同年代留下的，不要再混用：

| 编号 | 含义 | 用在哪 |
|---|---|---|
| **M1–M4** | 里程碑：地基 / 跨渠道表达 / 生活引擎 / 外部世界 | 主入口，给人看 |
| **任务 1–6** | 工程能力：回放、契约、提交、循环、表达、行动 | ROADMAP 的能力清单 |
| **阶段 A/B/C** | 交付批次：跨渠道表达 / 连续生活 / 未定义 | ROADMAP 的交付顺序 |
| **A1–A5 / B1–B5** | 阶段子项 | ROADMAP 的勾选项 |
| **PR-n** | 旧 R3 施工单 | 只作历史，不再新增 |
| **v15** | 数据库结构版本 | ARCHITECTURE / 配置 |

当前状态：**M1 已完成，M2 收尾中，M3 第一切片已接入、待真实使用验收，M4 以后**。旧 PR 的尾巴（PR-3.2b/3.2c、PR-5）并进 A5 和 B2/B5，不单开任务。

## 文档维护约定

- 状态变化先改 [STATUS.md](STATUS.md)，再改 ROADMAP / ARCHITECTURE；不要各写一份“当前状态”。
- 新增能力时更新对应主文档，避免另建一份完整架构或平行路线图。
- 阶段完成与测试结果记入 [HISTORY.md](HISTORY.md)，标明日期、版本和验证层级。
- 历史文档只追加、不重写结论；过期内容在文首加“历史”横幅。
- 新工作不再使用 PR 编号；需要追溯旧 R3 工作时才引用。
- 新文档与代码一样通过 Git 审查；构建目录和运行时 `app_data` 不作为仓库开发说明维护。
