# 文档索引

> 整理日期：2026-11-09。每个主题保留一个主要维护入口。

| 要了解的内容 | 文档 |
|---|---|
| 安装、构建、操作和常见问题 | [项目 README](../README.md) |
| 当前组件、记忆路径、v10、真实验证与限制 | [架构与当前实现](ARCHITECTURE.md) |
| 后续认知内核任务、依赖和验收 | [开发路线图](ROADMAP.md) |
| setting.json、identity.json 与配置默认值 | [配置参考](CONFIG_REFERENCE.md) |
| memory_recall/read、时间框架、状态与概览 | [记忆检索接口](MEMORY_RECALL_TOOL.md) |
| 既有 R3 入站、留存、授权和成员关系契约 | [R3 历史基线](MEMORY_V5_R3_BASELINE.md) |
| GPT-SoVITS HTTP 与备选模块调用参考 | [语音参考](GPT_SOVITS_REFERENCE.md) |
| 历史阶段、决策与文档合并去向 | [开发历史](HISTORY.md) |

架构文档记录源码现状，路线图记录待开发内容，配置默认值以 ConfigManager 源码核对。旧文档中的通过记录只适用于当时的版本；历史设计不能替代当前实现说明。

## 文档维护约定

- 新增能力时更新对应主文档，避免另建一份完整架构或平行路线图。
- 阶段完成与测试结果记入开发历史，标明日期、版本和验证层级。
- 新文档与代码一样通过 Git 审查。本次新增文件会显示为未追踪，提交时应和删除旧文档的变更一起纳入。
- 原先被 Git 忽略的四份本地记录已汇入 local/DEVELOPMENT_NOTES.md，继续保持忽略。构建目录和运行时 app_data 文档不作为仓库开发说明维护。
