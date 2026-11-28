# B5 验收准备（R12-3b-final 收尾后）

- 状态：R12 功能阶段收尾完成，**未启动正式自然运行**、未永久启用 actual、未修改生产配置/数据库。
- 基线：本文件随 R12-3b-final 提交；最终 HEAD 见提交记录与最终报告。
- 生产环境：v33 数据库、`identity.json`/`setting.json` 保持原样（`contact_daily_limit=0`、
  `snapshot_budget_bytes=4096`）；actual 默认关闭。

## 1. 最新可运行候选二进制

| 项 | 值 |
|---|---|
| 候选路径 | `build/stage-b/Windows_AI.exe` |
| 大小 / 时间 | 113,178,793 B / 2026-11-28 08:01:20（最终构建后） |
| SHA-256 | `6E2C52729522FC38E62A9B265B3145E769A1CB3B3005905A0B011085644A2998` |
| 构建 | Ninja Debug，Qt 6.11.1 / MinGW 13.1；全量构建通过 |
| 说明 | 候选二进制未复制到生产运行目录 `build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug`；生产 run 目录二进制/配置/库保持原样 |

## 2. 生产数据库与配置备份说明

生产运行目录：`build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug`。

| 文件 | SHA-256（只读核对） | 大小 / mtime |
|---|---|---|
| `app_data/memory/QianDaoMoZi_memory.db` | `8628AD01EA653B48B28DB6FE8EEEAD639AB9BAA69585B8EB0D221F2EEA020F19` | 110,641,152 B / 2026-11-27 19:38:49 |
| `app_data/config/identity.json` | `0E8EFAA3B588B555E5D5D63C5979D1E1D64C8FA9EF355E514A1C831E5F34033A` | 1,425 B / 2026-11-27 00:05:57 |
| `app_data/config/setting.json` | `91C90C2211BC7809EBD432E97630B56501110DCE259BF5C1F781D474FC9C99FF` | 3,408 B / 2026-11-22 18:28:36 |

已有整库备份（与生产库同哈希 `8628AD01…`）：
`D:\Desktop\mako_v33_pre_r12_2c_final_20261127_201827\`（含 `QianDaoMoZi_memory.db` + config + latest_log）。
另有历史备份 `QianDaoMoZi_memory.db.bak_*` 于生产 `app_data/memory/` 下。
**B5 开始前如需新备份**：直接复制生产 run 目录的 `app_data/`，不要运行迁移或写生产库。

## 3. actual 模式临时启用 / 恢复旧模式

- **启用 actual（正常方式）**：打开设置 →「认知与自主行为」→ 勾选
  「启用统一认知上下文（B5 实验功能）」→ 保存。
  - 写入 `identity.json` 的 `cognition.context_actual_enabled`，缺省 false；不新增 schema。
  - **保存后下一次独立认知运行生效，无需重启**；同一轮的主请求/工具轮/consult 使用同一模式。
  - 页面同时显示“配置值”（勾选框）与“当前运行状态”：未设环境变量时按配置，
    设了环境变量时显示环境变量覆盖。
- **启用 actual（临时覆盖）**：启动进程前设置 `MAKO_COGNITIVE_CONTEXT_ACTUAL=1`。
  - 环境变量只要显式设置就优先于配置，包括显式 `0`/`false`/`off`（表示强制关闭）。
  - 仅影响本次进程；不写配置、不迁移数据库；运行期修改环境变量对后续独立运行立即生效。
- **恢复旧模式（回退）**：
  1. 首选：在设置中取消勾选并保存（下一次认知运行生效）；或
  2. 设置 `MAKO_COGNITIVE_CONTEXT_ACTUAL=0`（显式覆盖）并重启；或
  3. 移除环境变量并重启。
  - 关闭后恢复旧上下文组装路径；无需回滚数据/配置（除非用户主动改了参数）。
- 启动后日志应出现 `[Cognitive][Actual]`；`actual_mode=true`、`projection` 含 A/B/C/D、
  裁剪/去重与 `prompt_tokens` 字段。
- **不建议**在 B5 期间用环境变量长期替代界面配置；验收结束按结论决定是否保留配置或回退。

## 3.1 认知参数设置界面（B5 补充）

设置 →「认知与自主行为」（`identity.json`）：

- **常用**：启用内部唤醒（OpenWake）、允许内部唤醒联系主人、共享每日预算占比、
  认知快照提示词预算、启用统一认知上下文（B5 实验功能）。
- **每日上限（三个上限相互独立）**：每日回顾上限 `proactive.daily_limit`、
  每日自主唤醒上限 `cognition.open_wake.max_per_day`、每日主动联系上限
  `proactive.contact_daily_limit`；界面与帮助文案明确区分，不互相替代。
- **高级设置（默认收起，点击“显示高级设置”）**：首次唤醒延迟、默认/最小/最大唤醒间隔、
  单次唤醒最多模型步数、到期后先让出的普通回顾次数、回顾失败退避秒数列表、
  连续失败暂停阈值。
- **actual 状态**：勾选框保存 `cognition.context_actual_enabled`；下方只读标签显示
  “配置值/环境变量覆盖”和当前实际运行开/关；保存后需重启生效。
- **保存机制**：沿用 `ConfigManager::writeConfigDocument` 与现有设置保存；
  只修改本页展示的字段，`cognition` 未展示字段与其他账户/API/隐私/渠道设置原样保留。
  权限缩减仍走既有 `reconcileActionPermissions` → action context epoch 撤回；
  关闭再开启权限不会让已失效的在途行动复活。
- **范围/校验**：沿用 ConfigManager 的夹紧规则（如快照预算 2048..8192、
  唤醒上限 0..96）；不新增另一套规则、不新增数据库 schema。

## 4. 日志采集

| 日志前缀 | 用途 |
|---|---|
| `[Cognitive][Actual]` | 每轮实际投影：input/baseline chars、dedup_rows/full_rows/omits、A/B/C/D、dedup_saved、budget_omitted/truncated、privacy、budget |
| `[Cognitive][Shadow]` | 新旧对照（shadow 开启时） |
| `[Snapshot]` | 每域 queried/eligible/serialized/omitted 与预算 |
| `[Review]` / `[Wake]` | 回顾领取/结果/退避/来源 |
| `[Semantic]` / `[Proposal]` | verifier、提案状态、失败闭合 |
| `[Outbox]` / `[Receipt]` | 唯一发送、回执/历史投影、unknown 不重发 |
| `[Reminder]` | 提醒调度/拒绝原因 |
| 运行时日志 | `app_data/logs/logs_*.txt`（候选 run 与生产 run 分开） |

采集：保留 B5 窗口内所有日志与 `[Cognitive][Actual]` 行；按会话/行动 key 关联即可，无需新增埋点。
禁止在日志中记录完整私人正文用于分析；只记录 ref/摘要/计数。

## 5. 2–4 小时受控使用计划（有人值守）

前置：备份生产 `app_data`；候选二进制/配置哈希记录；确认 actual=1 后启动的是**隔离 run 目录**
或明确记录“将生产 run 转为候选版本”的授权；不建议未备份直接替换生产二进制。

| 时间 | 场景 | 观察点 |
|---|---|---|
| 0–20min | Pet 私聊自然闲聊 5–10 轮 | 回复自然度、上下文开销、是否重复/复述、actual 日志无 error |
| 20–40min | QQ 私聊 + 跨渠道更正（先说 A 后更正 B） | 有权限入口采用最新认识；不发旧状态追问；无私人正文泄露到群 |
| 40–70min | 活动：开始写作/继续/暂停/恢复 | 是否延续同一活动与草稿版本；不无故重启 |
| 70–100min | 提醒：创建/读取/取消/到期 | 不重复提醒；取消后不发送；回执状态正确 |
| 100–140min | OpenWake：允许联系 + 安静两种 | 联系经 verifier/Outbox/回执；安静时内部完成且不发送 |
| 140–180min | 群聊主人 + 群聊第三方 | 群内主人不得自动获得私人 Pet/私聊原文或内部状态；第三方零私人资料 |
| 180–240min | 收尾 | 汇总指标/异常；决定继续 7 天观察或回退 |

指标记录：实际 input chars/bytes、prompt_tokens、模型请求次数、延迟、失败率、重复正文/预算省略、
duplicate 投递、隐私过滤/泄漏、行动回执状态。**不以主动联系/活动完成次数作为目标。**

## 6. 后续 7 天自然使用观察计划

每天只记录、不人为纠正：
- 日常交流是否自然、是否重复提醒/复述用户话语；
- 跨渠道最新认识是否正确（更正后不再用旧状态）；
- 活动/念头是否能延续（同一 ref/version/草稿）；
- 联系 vs 安静是否合理；错误承诺/错误执行状态；
- 隐私泄露（群/第三方）、重复投递、失效行动发送；
- 上下文开销、模型调用次数、延迟、失败率、截断/网络错误。
第 7 天汇总并按停止/回退条件判定。

## 7. 停止与回退条件

立即停止 B5 并回退（去 actual 环境变量 + 重启旧二进制）的条件（任一）：
1. 发现私人正文/来源泄露到群聊、第三方或无权入口；
2. 同一行动重复投递、失效行动发送、错误执行状态/错误承诺；
3. 行动核验被绕过（未走权威证据/VerifierBinding）；
4. 提醒重复发送或取消后仍发送；
5. 系统性上下文膨胀/延迟/失败率显著高于基线（可复现）；
6. 模型请求数因 actual 额外增加，或数据库/配置被意外修改；
7. 日志/回执出现 unknown 却自动重发。
回退后保留日志、`app_data` 与哈希记录，提交问题清单，不自行改生产库。

## 8. B5 关注清单（最终）

- 日常交流自然度、跨渠道最新认识、重复提醒/复述。
- 活动与自身念头连续性、联系/安静合理性。
- 错误承诺、错误执行状态、隐私泄露、重复投递、失效行动发送。
- 实际上下文开销、模型调用次数、延迟及失败率。
- **不以主动联系次数、活动完成次数作为强制指标。**

## 9. 后续架构收敛清单（B5 后，不在本轮做）

1. `CURRENT_UNDERSTANDING` / `MEMORY_DATA` 正文唯一序列化位置迁移。
2. Matter consult / `memory_recall` / `memory_read` 正文与引用统一。
3. 旧正文位置、重复适配层、临时证据表（`m_run.evidence`）收敛。
4. Shadow 对照分支降级为只读统计。
5. 模块解耦/合并/旧机制退役（按用户决定集中处理）。
6. 不新增独立 Planner；预算仍由确定性用途优先级决定。

## 10. 边界声明

本轮只准备 B5：未启动生产应用、未永久启用 actual、未修改生产配置/数据库、未清理历史、
未进入 R13/Vision/后台通用 tool loop/完整 Attention 重构。

## 11. 候选版本验证记录

- 全量构建通过；**ctest 24/24 通过**（`build/stage-b/b5-settings-final-ctest.log`，102.23s）。
- `tst_settingspages 11 passed`（导航合并/三个上限单入口/高级诊断搬迁/非法区间拒绝/
  有效值归一显示/无修改不写入/未展示字段保留/actual 状态/环境变量）；
  `tst_memoryagent 127 passed / 6 skipped`（含 actual 配置热读取与环境变量优先级）；
  `tst_cognitivecontext 30 passed`；`tst_memorymanager 157 passed / 1 skipped`；
  `tst_actionoutbox`/`tst_ttslifecycle`/`tst_qqdelivery` 覆盖权限 epoch、TTS 安全切换、
  QQ 重连不重复投递。
- 生产库/配置哈希与 mtime 未变；候选版本尚未复制到生产 run 目录、未启动。

## 12. 配置生效策略与热更新（B5 最终）

保存设置时，AppController 比较**保存前后的有效配置快照**（`effectiveSettings()` +
identity.json），只调用受影响模块：

| 变化 | 运行更新 | 生效时机 |
|---|---|---|
| 日志目录/分次 | `RuntimeLog::configure` | 立即 |
| DeepSeek API Key | `LLMService::setApiKey` | 新请求；不重建服务 |
| DashScope Key/URL、索引/向量/维度 | `MemoryIndexWorker::reconfigure` | 安全批次边界；相同配置不打断 |
| 候选数量/重排数量 | 无需 reconfigure | 下一次检索 |
| TTS 模式/路径/权重 | `TTSService::reloadProvider`（内部相等性判断/安全切换） | 安全任务边界 |
| QQ 连接启停/地址/令牌 | `setConfig` + 重连 | 立即；unknown 不自动重发 |
| QQ 普通回应/群权限 | `setConfig` 仅更新配置 | 下一条消息 |
| 群成员相关配置 | `GroupMembershipService::start` | 立即刷新 |
| 主人/自身账号、盲区、跨渠道、主动/OpenWake 允许联系、长期记忆 | `reconcileActionPermissions` + epoch | 立即，在途行动失效 |
| 时间/退火 | `applyTimeSettings` + `TimeManager::start` | 立即 |
| 外观跟随外部情绪 | `setFollowExternalMood` | 立即 |
| 认知/主动/记忆/检索普通参数 | 无需模块重载 | 下一轮独立运行/调度边界/下一次检索 |
| actual 开关 | 读取当前有效配置 | 下一次独立认知运行；同一轮不混用 |
| prompt.txt | 每次请求读取 | 下一次请求 |

未变化字段/无修改保存不触发任何运行更新（`tst_settingspages::noChangeSaveDoesNotApplyOrRewrite`
与 `settingsDiffPlanOnlyMarksAffectedModules` 覆盖）。

环境变量 `MAKO_COGNITIVE_CONTEXT_ACTUAL` 显式设置（含 0/false/off）优先于配置；
测试 setter 优先于两者。配置值不再永久缓存。
