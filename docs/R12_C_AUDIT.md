# R12-C 认知架构审计与实施方案

## 基线

2026-11-28，HEAD 为 cacf122fa8b82c2ad218fc6751a99fcac15ab206，开始审计时工作树干净，跟踪 origin/main。B5 实例运行于 Debug 目录，磁盘 Windows_AI.exe 修改时间 09:38:30，与本轮日志开头吻合，SHA-256 为 f0683f806728b0985593bb5cf83a99683887fc66f0ca641025bb8cc73a1fa3a1。缺少把该二进制绑定到具体 Git SHA 的构建清单。生产库以只读方式确认为 v33，仍有 private_reflection、reflection_contact、review_failure、open_wake、internal_activity、action_proposal、action_outbox 等表。

当前磁盘设置为每日回顾 288、每日联系 0（不限）、回顾间隔 60 秒、空闲 120 秒、认知快照 4096 字节。OpenWake、其联系授权、Actual、跨私聊读取、向量与重排开启，回忆为 tool/native。附件是待核实分析，以下依据本地源码及日志。

## 架构与历史机制

| 入口或机制 | 现状与退役条件 |
| --- | --- |
| 前台 | [SessionRouter::requestReply](../src/app/sessionrouter.cpp#L90) → [AppController 与 CognitiveCoordinator](../src/app/appcontroller.cpp#L870) → 记录及批处理 → LLMService → 渠道投递。前台 [Actual/Shadow](../src/cognition/llmservice.cpp#L1456) 仍有旧提示构建回退。 |
| 主模型回忆 | LLMService [工具暴露](../src/cognition/llmservice.cpp#L563) → [MemoryRecallTool::execute](../src/memory/memoryrecalltool.cpp#L329) → MemoryExecutor → 统一检索。native 与 JSON envelope 有实际实现，均使用共享检索、needs 和时间语义。 |
| 独立 Planner V1/V2 | [legacy 分支](../src/cognition/llmservice.cpp#L1697) 调用 shouldRun，进入 [Planner 协议切换](../src/memory/memoryagent.cpp#L1094)。本机 tool 模式不走，legacy 仍有生产入口。C2 先处理历史配置的归一、兼容工具和近场感知，然后移除专属 HTTP、解析及门控。MemoryExecutor、needs、结果预算和时间解析保留。 |
| 后台 | [CognitiveCoordinator 空闲门控](../src/app/cognitivecoordinator.cpp#L228) → [PrivateReview 活动/关注/新源与 OpenWake 调度](../src/app/privatereview.cpp#L540) → [后台 Actual/Shadow](../src/cognition/llmservice.cpp#L2319) → 各领域回写。Actual 在四类后台任务与前台均通过上下文、引用、权限和预算验收，再将 Shadow 移到诊断，并移除旧正常路径。 |
| 联系与投递 | OpenWake 和内部成果使用 [ActionProposal 核验绑定](../src/memory/memorymanager.cpp#L11269)，由 ActionCommitter 和 Outbox 提交。Reflection/Concern 的 basis 与 motivation/claims 仍经 [reflection_contact](../src/memory/privatereflection.cpp#L1406) 提交。旧域兼容读与停止新写分开。迁移需覆盖已有回执、unknown、重复 action_key、撤权和发送前复验。用户提醒依其委托和有效期独立运行。 |
| 活动 | 新活动与旧 thought 共存，[热启动分享恢复](../src/app/privatereview.cpp#L1191) 等有实际消费者。停止旧写入前确认暂停、恢复、撤回、分享和回执均有统一入口。旧草稿、引用和历史状态继续可读。 |

## 附件逐项复核

| 问题 | 已确认 | 待验证 |
| --- | --- | --- |
| 611 连续重领 | B5 日志显示 09:45:31 至 09:52:31 共八次领取，版本 7–14。前七次可见失败为 invalid_or_stale_proposal，counts=false、failures=0、next_retry=0。与 [PrivateReview::errorCountsAsFailure](../src/app/privatereview.cpp#L349)、[recordFailure](../src/app/privatereview.cpp#L385)、[deferPrivateReflection](../src/memory/privatereflection.cpp#L1695) 和 [领取 SQL](../src/memory/privatereflection.cpp#L258) 对应。零退避允许下个正常调度周期重领。 | 第一次落库触发哪个判断无法由日志恢复。第八次领取和前七次失败是不同事实。 |
| 内外结果耦合 | [finishPrivateReflection](../src/memory/privatereflection.cpp#L1292) 混合来源、prior、证据、权限、SQL、CAS 等失败为 bool。外发 timing/reason/message 校验及联系行写入仍可能回滚内部事务。明确 withheld 有能继续落库的分支。09:46 verifier 返回 HTTP 200、完整正文和 diagnostic=ok，随后仍见内部提交失败。该诊断本身不能证明语义通过。 | 不能把八次领取都归因于 verifier 截断。需具体首个拒绝码。 |
| 1024 token 用尽 | OpenWake 09:44 以及 Reflection 多次显示 HTTP 200、length、completion_tokens=reasoning_tokens=1024、正文 0，同一日志也有通过样本。[请求预算](../src/cognition/llmservice.cpp#L2760)、[失败关闭](../src/cognition/llmservice.cpp#L3707)。 | 实际 provider 的推理参数支持情况、可用预算与语义质量需隔离对照。 |
| 快照饥饿 | [SnapshotBuilder 固定裁剪顺序](../src/cognition/cognitivesnapshot.cpp#L547) 与 [Actual 投影二次裁剪](../src/cognition/cognitivecontext.cpp#L2584) 均可能挤压动机、推断及自我表达。 | 需用每域候选/授权/入 prompt/省略计数验证各入口长期饥饿。单轮省略不足以证明。 |
| 频繁索引 | 有 250ms 合并及在途 pending，B5 多次扫描仅一条。[MemoryIndexWorker::schedule](../src/memory/memoryindexworker.cpp#L61)。 | 先测事务/连接、外发次数、缓存命中和可检索延迟，才决定调度改动。 |
| 跨渠道 | [背景群窗口](../src/cognition/llmservice.cpp#L804)、[后台检索](../src/cognition/llmservice.cpp#L1854)、[Reflection 可见性](../src/memory/privatereflection.cpp#L45) 和 [入站资格](../src/memory/memorypolicy.cpp#L156) 用途不同，授权相同不保证投影相同。 | 区分未记录、未召回、裁剪、禁止转述。用同一依据逐身份/渠道重放。 |

## 配置生命周期

[逐项设置索引](R12_C_CONFIG_INVENTORY.md) 覆盖界面声明的 111 项，含 UI 名称、默认、类型、范围、帮助位置及解析与消费者定位候选。[原有 B5 设置矩阵](B5_SETTINGS_MATRIX.md) 提供已归纳的服务和热更新时机。索引不是逐项动态验收记录。同名叶键、专用控件和未映射项尚需在 C1 逐项确认。

| 分类 | 本轮校准 |
| --- | --- |
| 正常生效 | 角色提示、日志路径/轮换、时间/表现、身份、记忆窗口和摘要各有运行入口。UI 只保存编辑页且保留未知字段，[保存](../src/ui/settingspages.cpp#L1134) → [ConfigManager 重载](../src/config/configmanager.cpp#L602) → [差异应用](../src/app/appcontroller.cpp#L845)。 |
| 条件生效 | 视觉依赖有效 provider 与消息附件。语音模式、模型文件与安装路径依 provider，参考目录空值走本地默认。QQ 连接变化需局部重连，记录/回应范围在入站按各自规则判定。索引切换在工作边界应用，回填空索引与增量补向量是两件事。 |
| 运行含义与帮助不符 | rerank_limit 在关闭重排时仍用作返回截断，[MemoryRetrieval](../src/memory/memoryretrieval.cpp#L588) 以及 602/620–636 行。concern_retention_days 已用于[定时复查](../src/memory/privatereflection.cpp#L641)及[重试保留期](../src/memory/memorymanager.cpp#L6257)，不会按天删除历史。OpenWake 上限受共享回顾和占比约束，[预算](../src/memory/memorymanager.cpp#L6680)，当前有效上限 12，UI“三个上限相互独立”需修正。Actual 的“需重启”说明应复核每轮读取与进程环境覆盖，[读取](../src/cognition/llmservice.cpp#L4076)。 |
| 旧模式 / 兼容 | agent_mode 真实切换架构，plan_v2/trigger 具备旧 Planner 消费者。agent_timeout_ms 传入 [旧 scope](../src/cognition/llmservice.cpp#L1344)，共享 [MemoryExecutor::execute](../src/memory/memoryexecutor.cpp#L104) 也消费 scope 的 timeout，需分清工具自己的默认值与该 JSON 配置是否传入。JSON envelope 与 native 仍有兼容价值。 |
| 仅诊断 / 实验 | local_hash_test 为诊断 provider，Actual 为 B5 实验并可由环境覆盖，[开关](../src/cognition/llmservice.cpp#L4056)。详细日志属于诊断功能。 |
| 已解析无运行消费者 / 尚未实现 | qq.group_retention_days [读写](../src/config/configmanager.cpp#L291) 与测试存在，未找到运行清理消费者。memory.retrieval.legacy_fallback 在 src/tests 无读取，是旧配置残留候选。 |
| 新发现 | 关闭 private_writing 时，活动领取已检查权限，普通 Reflection 仍可能从 [wakeNonOpen](../src/app/privatereview.cpp#L615) 领取，随后在 [回顾检索回调](../src/cognition/llmservice.cpp#L1885) 拒绝。需以隔离矩阵确认是否出现消耗额度和执行检索后失败。cacf122 的跨渠道差异路径已修，勿把历史设置写错的问题重复归为现状。 |

## 硬编码治理

- [MemoryAgentPlanner::isMemoryQuestion](../src/memory/memoryagent.cpp#L1520) 是认知调用的词典门控，随旧 Planner 退役。近期群背景的 [isRecentCue/isHistoricalCue/detectMode](../src/memory/recentcontext.cpp#L38) 也决定是否提供基础感知。C2 改用会话、时间、权限、预算确定当前窗口，授权历史由主模型按需查。
- [extractTerms](../src/memory/recentcontext.cpp#L59) 仍承担词面检索、事项候选与证据补查。保留词面评分，不能用无字面命中判定不存在相关记忆。补相似改写与指代回归。
- [Matter 提交](../src/memory/matterstore.cpp#L284) 的 contains(quote) 与时间原文锚点用于来源验证。保留引用、版本、来源、枚举及状态约束。由主模型判断事件同一性和修订含义。
- [ActionPolicy](../src/cognition/actionpolicy.cpp#L277) 的不确定表达与事实断言词表/正则可能影响语义判定，C3 用真实正反例验证覆盖后收敛。证据 ref 格式、协议、时间解析和[管理员自定义隐私](../src/memory/memorypolicy.cpp#L35)保留。
- [后台 prompt](../src/cognition/llmservice.cpp#L1915) 与旧/新投影中的重复要求需要逐入口检查。合并重复协议，验收自然表达与证据支持。未完成逐条提示词行为标注，不能认定某一措辞已造成实机重复。

## C0 具体计划

1. 将 Reflection 提交 bool 改为有阶段、原因及 disposition 的结果类型。分别报告形状、来源、prior、证据/权限、CAS、DB 错误，保留最初失败位置。日志仅记录短码及来源 ID、version、run_id。涉及 [finishPrivateReflection](../src/memory/privatereflection.cpp#L1292)、声明及协调用方。
2. 区分 yield、晚到、依赖变化、可重试错误与外发 withheld。yield 保持原调度。晚到回调不得操作新版本，依赖变化等新版本，短暂失败采用已有有界退避。用 v33 已有失败、租约和状态字段生效。勿改全局回顾间隔。
3. 内部通过时完成 CAS，独立记录外发命运。外发形状不合法、拒绝或截断不能抹去合法内部认识。C0 先处理事务中可选联系分支及审计，C3 再统一为 ActionProposal。保持重启、幂等和发送前复验。
4. 使用脱敏真实样例比较核验请求配置，覆盖有依据、矛盾、遗漏、越权及纯动机。衡量可解析正文、准确性、延迟和用量。保留每行动最多一次 verifier、零次 repair。未经实测不预设提高 token 或切换 provider。
5. 回归复现同轮/跨重启重领、prior 修订、旧回调、用户插话、罕见 SQL 失败、外发拒绝和合法主动联系。对照模型请求数、有资格的任务次数与回执一次性。涉及 tst_memorymanager、tst_memoryagent、tst_concern 与 tst_actionoutbox。

## C1–C4 依赖和验收

| 阶段 | 前置、代码与验收 |
| --- | --- |
| C1 | 基于 C0 诊断和配置索引，统一 UI 与 ConfigManager 的默认、类型、单位、适用条件和有效值展示。保持局部重连与任务边界。修改 settingspages、configmanager、settingsapplyplan，测缺省、老配置、未知键、热更新和回退。 |
| C2 | C1 明确兼容配置解释后收敛主模型回忆。修改 llmservice、memoryagent、memoryrecalltool 与 recentcontext，保留 MemoryExecutor。测无关键词的群背景、同义问法、不同时间范围及无权限。统计前台独立 Planner 调用减少情况。 |
| C3 | C0 回顾结果独立，C2 工具稳定后按入口接入行动与认知投影。按身份/渠道/来源/用途分别测内部可见、公开表达、工具补查。修改 cognitivecontext、cognitivesnapshot、actionpolicy、privatereview及 ActionCommitter，保留提醒、活动及回执。固定 4096 字节对照每域覆盖。 |
| C4 | C2/C3 验收后按职责拆 llmservice 的请求构造与核验、memorymanager 的迁移和事务域。删除经证明可退役的入口，用旧数据库副本和原生入口回归。避免机械追求行数或文件数。 |

## 数据保护、回退及审计范围

后续复现使用隔离数据库和 mock sender。发布前冻结 Git、可执行文件、配置哈希与一致性备份，检查 SQLite 完整性、外键、关键表计数与 history/outbox 幂等。回退优先二进制和开关，绝不覆盖运行中新增的真实历史。维持 v33，涉及迁移需单列条件、风险及回退并获评审。保持既有回顾、联系、权限、角色原文。私人正文、配置和凭据留在本地。

本轮只产出审计文档和设置索引。未执行构建、模型请求、真实投递或生产程序启动。部分配置消费者仅完成静态映射，完整热更新与行为矩阵留待实施验收。
