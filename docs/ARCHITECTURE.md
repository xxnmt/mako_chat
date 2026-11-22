# 架构与当前实现

> 更新于 2026-11-21。检索运行与验证记录见 [P5 交接](RETRIEVAL_REFACTOR_STATUS.md)。
> 先读 [当前状态](STATUS.md)；后续任务见 [开发路线图](ROADMAP.md)；入口见 [文档索引](README.md)。
> 本文只描述当前实现。文中 PR/R3 编号是历史工作包，不代表当前路线图。
> 当前表达请求直接接收统一检索返回的证据、场景与当前认识，可按需调用记忆工具。

## 组件与职责

当前实现已接入阶段 A 的消息运行协调。CognitiveCoordinator 协调前台对话和到期的私下写作；后台认识更新沿用既有调度。原始愿景中的持续生活、成长人格、内在情绪和自主社交尚需后续实现。近期开发顺序以路线图的阶段 A、B 为准。

聊天请求在统一发送入口附加 expressionOutputContract，保持直接表达、工具后表达和重试的最终 JSON 契约一致。QQ 使用与 TTS 相同的句子单元；QQProtocolClient 对 send_* 动作串行等待回执，UUID echo 区分每句，断线清队列。提交成功、协议端响应和最终送达仍须区分；现有 handed_off 只记录交给渠道，详见交接中的失败语义。

QQ 消息段数组保留图片所在位置、消息 ID 和明确引用的目标。前台模型可见这些关系和 `[图片]` 占位符。启用识别后，ImagePerception 将本轮获准的图片与同批文字发送给视觉模型，成功的观察按事件和段位置进入对话证据表。图片地址和桌宠文件路径留在传输层。跨轮视觉证据留存列入[阶段 B 下一批交付](ROADMAP.md#下一批交付与验收)。

前台输入先按既有策略分条留存。协调器对同渠道、会话、身份、引用和留存策略的相邻输入短暂等待，模型按接收顺序一次读取。较早的输入在历史中标为 `grouped` 并关联下一条，回应或安静决定写在末条；短期历史以首条为界。生成开始后的补充进入后续队列。

QQChannelAdapter 为每个文字单元传入本地模拟时长，与 PetAudioSink 的模拟播放共用 textplaybacktiming（250ms/字符单元，最少一秒）。QQProtocolClient 使用单调时钟协调回执和剩余阅读时间；实际音频、MockTTS 合成等待及非发送类 OneBot 查询不受影响。debug_full_logs 下消息打印位于最终发送入口，包含追加的服务端输出契约。

阶段 B 的目标架构包含持续活动、自主回顾和主动私聊，详细任务见路线图 B1–B5。协调器提供事件、运行归属、注意力与版本管理。模型依据经历和已有关注选择继续活动、产生新念头、联系、延后或休息。调度器控制唤醒条件和资源预算，执行端核验渠道能力与表达条件。活动进度、念头和实际行动结果需要持久化，并处理取消、删除、重试与迟到结果。写作与独处回顾共用 PrivateReview 和同一协调入口。协调器每分钟检查资格，符合条件时从库中领取一个到期活动。领取和预算记账在同一事务中完成，领取后使用新版本接收模型结果。新消息和正常退出会取消在途请求，保存 deferred_reason 并释放到期时间。上次提交的 draft、progress 和 next_step 随下次请求进入模型。异常退出沿用领取时的 30 分钟保留期，期满且预算允许后可重新领取。主动分享另行核验联系开关、时段和目标渠道。

v18 的 private_reflection 保存回顾状态、尝试版本和茉子自己的看法，并与来源轮次关联。到期活动优先，否则从最近 24 小时符合留存条件的主人私下交流中取一条。休息和留下想法都会记录；完成后跳过同一来源。删除来源会清理相应回顾。下一次主人私下对话可读到最近一条符合当前权限的自身联想。进行中活动仅在明确提交 reschedule、时间和原因后改期，resume 用于恢复暂停活动。

v19 将回顾的联系决定和投递状态保存在 `reflection_contact`。立即联系在发送前按来源、版本、当前配置和私下目标领取一次。进程意外终止后，已经领取的消息不会自动重发。QQ 响应和超时凭 echo 关联对应决定，桌宠气泡仅记录本地交接。删除回顾也删除联系记录。

v22 回顾先经 MemoryRetrievalService 检索来源轮次之前的主人发言，再按来源读取原话、发言时间和相关旧关注。网络检索与前台共用 Provider 配置并使用当前实例的数据库，回顾拥有独立的取消标记与模型请求。模型可继续定向补查一次，也可休息。模型返回的 prior_action 只作用于本次提供的旧关注；提交时复核依据和旧关注版本。reflection_change 保存新旧来源和修订或解决动作，旧记录与联系结果仍可追溯。删除更正来源会清理关联，使原有关注在其依据仍可读时重新可见。

| 模块 | 当前职责 | 源码入口 |
|---|---|---|
| 应用组装 | 创建服务、连接信号、渠道投递与分享条件检查 | appcontroller.h/.cpp、main.cpp |
| 输入与身份 | 渠道输入、主人账号归一、事件/运行 ID、时间、RequestMeta | sessionrouter.*、utterance.h |
| 模型调用 | 提示组装、内部判断、聊天工具循环、重试、后台提取与回顾 | llmservice.* |
| 提示组装 | 按当前场景组装身份、会话历史与不可信记忆资料 | promptassembler.* |
| 数据存储 | SQLite 迁移、对话、事件、人物、摘要、事实、认识与念头 | memorymanager.*、historyturn.h |
| 按需检索 | recall/read 协议、时间解析、执行与概览；legacy Planner 保留 | memoryrecalltool.*、memoryexecutor.*、memoryagent.*、memoryoverview.* |
| 时间语义 | 时间框架、范围、节日与时区解析 | memorytimeframe.*、memorytimerange.*、timeframeresolver.*、festivalcalendar.* |
| 群记忆整理 | 获准入站事件分段、后台摘要与 claims | raweventprocessor.*、interactionsegmenter.* |
| 授权与输出 | 入站留存、来源策略、claims 授权、敏感过滤与输出检查 | memorypolicy.*、memorysourcepolicy.*、memoryaccesspolicy.*、memoryoutputguard.* |
| QQ 连接 | OneBot WS、重连、消息解析、白名单、成员关系 | qqprotocolclient.*、qqmessageparser.*、qqreplypolicy.*、groupmembershipservice.* |
| 渠道表现 | Pet 气泡与音频、QQ 文本投递 | ichanneladapter.h、petchanneladapter.*、qqchanneladapter.* |
| 语音 | provider、参考音频、流式/文件播放、GPT-SoVITS 进程 | ttsservice.*、apittsprovider.*、ttsreferencemanager.*、petaudiosink.*、streamplayer.*、fileplayer.*、ttsprocessmanager.* |
| 角色表现 | 四维立绘、全局表现标签、昼夜服装与退火 | appearancemanager.*、globalmoodstate.*、timemanager.* |
| 窗口与设置 | 角色、气泡、聊天、设置和位置锚定 | characterwidget.*、bubblewidget.*、chatwidget.*、settingswidget.*、anchormanager.* |
| 配置 | setting.json、identity.json、配置默认值和保存 | configmanager.* |

上述源码路径相对仓库根目录。GlobalMoodState 是表现标签状态，独立的内部情绪成长模型仍待开发。

## 正常对话路径

渠道输入经 SessionRouter 生成 RequestMeta 后，AppController 将输入交给 CognitiveCoordinator，由它保存输入并排入前台队列。实际启动时重读当前会话历史，再调用 LLMService。LLMService 先准备跨渠道证据，再组装最终上下文并进入聊天及记忆工具循环。前台结果携带对应输入和元数据，AppController 将正文交给原渠道，协调器在渠道交接时回填对应输入行，控制器按阈值触发后台记忆提取。

LLMService 仍保留单份活动请求状态，当前应用依靠前台串行队列保证其调用边界。成功结果携带上下文不等于服务已支持任意并发调用。MemoryAgent 的结果及失败带请求 ID；主网络回复校验运行代次。原生工具拒绝时可回退 JSON Envelope，输出异常与空回复执行有限重试。

Pet 走语音合成和音频 sink，QQ 走文本发送。QQ 情绪可按配置影响立绘。播放收尾通过轮次序号约束退火，昼夜与退火属于表现管理。

## 记忆与持久化

当前 SQLite user_version 为 16。基础表包括 chat_history、character_profile、episodic_memory、long_term_summary、relationship_state；多渠道之后增加 raw_events、memory_cursor、interactions、人物身份、claims 和群成员关系。完整字段以 MemoryManager::initDatabase 为准，v10 在 chat_history 中增加 state_updates、thought、thought_version、thought_result、review_after，并增加 thought_review_log 与索引；v15 增加 memory_chunks、memory_chunk_sources、memory_embeddings、memory_index_state。

会话短期历史按 channel/session_id 分桶。稳定档案和关系可内联；tool 模式下历史事实主要按需检索。统一检索证据与 currentUnderstanding 另行接入最终请求前的上下文准备。原始事件、聊天轮次和派生证据有各自删除边界，删除聊天记录不会自动删除独立 raw_events。

既有 R3 披露工具契约见 [R3 基线](MEMORY_V5_R3_BASELINE.md)，工具 JSON、状态和时间框架见 [检索接口](MEMORY_RECALL_TOOL.md)。R3 文档中的 v9 快照只描述当时路径，新增内部证据路径不能套用旧矩阵推断其全部行为。MemoryAccessState 定义四个枚举，当前 evaluate 实际返回 Shareable 或 Forbidden；KnownOnly/OwnerCandidate 不能据枚举定义视为完整实现。

## 2026-11-12 归属、场景与文字活动

- **承诺归属**：`currentUnderstanding` 为每个事项保留 `self_report`（最近一次本人原话、版本、`source/N`、`speaker_role=user`）。`prepareReply` 把 `self_report` 放进 `CURRENT_UNDERSTANDING` 目录；证据元数据补 `speaker_role` / `source_kind`。`attribution_check` 分三层：编号有效、说话人正确、语义是否蕴含（`semantic_review_required`，由验收样本判断）。
- **关心收尾**。表达请求要求分清用户回应的具体事项，保留拒绝和完成状态。
- **场景事实**：`PromptAssembler::buildSituationBlock` 注入 surface / speaker / audience / about / modality / privacy。它只说明在哪里、对谁、能用什么形式，不要求不同渠道换一种说法；`about` 未确认时写 `unknown`，不把当前对话对象当成事情主体；pet 只有在 TTS 为 `api` 时才标 `voice+text`。
- **文字活动**：复用 `chat_history.thought` + `thought_result`，不新增表。`finishThoughtReview` 支持 `paused` 与 `progress/next_step/paused_reason`；`applyThoughtAction(id, expectedVersion, action)` 条件更新并递增 `thought_version`，迟到写作回调因版本不匹配被丢弃。`draftReadyToShare(activityRef, version, draft)` 在发送前再次检查 `canShareThought`。
- **开关拆分**：`proactive.private_writing` 控制后台写作/回顾，`proactive.enabled` 控制主动联系/分享；`quiet_hours` 只暂停后者。

## 统一检索与记忆感知

- `memoryevidence.h` 定义 `MemorySituation`、`MemoryEvidence`、`RetrievalRequest`、`RetrievalResult`；`memoryretrieval.h` 定义 `MemoryRetrievalService`；`memoryembedding.h` 定义可选向量 Provider。
- `LLMService::askDeepSeek` 的自动证据准备调用统一服务：Pet、QQ 私聊、群聊使用同一入口；`currentUnderstanding(..., includeAudiencePrivate=true)` 让表达模型保留相关私人背景。
- v15 的 `memory_chunks` / `memory_chunk_sources` / `memory_embeddings` / `memory_index_state` 是可重建索引；新 raw/chat 写入同步增量索引，`--rebuild-memory-index` 用于旧库整库回填，`--memory-index-status` 查看状态，结构化来源单独读取原表。
- 索引路径当前为BM25 与可选向量召回，经 RRF 合并后，按配置使用本地或 DashScope 重排。LocalHashEmbeddingProvider 可用于测试。
- `MemoryAgentTools` 已补充助手表达、说话人和派生字段，并允许 Pet 使用工具；六类搜索来源均通过统一服务取得候选，工具继续负责授权和结果句柄。

## 实际验证与当前限制

2026-11-08 本机启动日志确认记忆库从 v9 升至 v10、QQ 连接和成员快照成功。当天实际对话中，QQ 私聊的用餐安排曾被 Pet 正确召回；同一安排在 Pet 中被描述为可能改期后，QQ 仍回答原安排，追问调整也未找到更正。群聊曾出现 memory_recall 参数错误及零结果，最终回复把检索不足说成没有记住。因此当前版本应标为实验性连续性实现，尚未达到跨渠道认知一致的验收标准。

最近一次开发验证中，Windows Debug 构建曾成功，15 个自动测试套件通过。后续召回排序修改也通过全部套件；运行中的原名可执行文件会阻止覆盖链接。测试覆盖的是受控输入下的数据与请求行为，不能据此宣称真实模型已能稳定理解更正。下述已实现路径表示源码已接入。

v11 的认识更新独立于对话回复。语义关联仍由模型负责，每次最多选取该主体 48 个事项，按输入词匹配及修订顺序排序。历史证据检索仍有数量上限，群聊表达按相关性与预算读取证据。连续两条消息之间可能尚未完成后台提交，当前状态按已提交版本读取，提取器也可能将更正误判为无变化。

当前排序同时保留主题相关性与本人时间线的候选机会，未针对私聊设置固定的一天窗口。时间字段包含消息时间、事件时间和摘要整理时间，现有聚合候选尚未完整区分这些语义，这是后续重构需要处理的缺口。

2026-11-11 使用日志确认 Pet 的鲜奶发言进入了主人私聊的资料与当前认识。回答将此前自述与此刻持续状态的推断一起表达为猜测。随后同一主人在群里询问近况，群聊认识投影为空，内部回想仍只传递三个决定，历史工具范围限定本群。该路径存在内部认识无法延续到表达的问题。另有夜间问题落入凌晨检索区间的异常，完整工具意图尚需诊断。这是阶段 A 改动前的基线。现在群聊能接收按听众筛选的认识；“现在”的检索区间若不含请求时间会被拒绝，避免把晚间误当作当天凌晨。

这份设计从一个具体行为出发。主人在群里说过脚踝疼，后来私聊说好了。回到 Pet，她应该能接着聊康复后的生活。回到群里，她也应当放下旧的担心，同时顾及谁在听。下面区分已经接入程序的行为和后续能力。

## 已实现的运行路径

1. Pet 和 QQ 经 SessionRouter 识别人。每次有效输入生成事件 ID、运行 ID、接收时间和发生时间。CognitiveCoordinator 顺序处理前台请求，实际开始处理时读取当前会话的历史。重试和记忆工具属于同一运行，成功结果带回原输入和渠道元数据。
2. 符合保存策略的入站消息在 SessionRouter 交给 AppController 时进入 cognitive_source。event 对同一内部事件去重，run 用于诊断，reported 是原消息时间。背景请求为当前主体读取事项及原始和最新依据，提出无变化、需更多证据或更新。matter_revision 用独立事项 ID 和不可复用的来源序号标识版本，保存消息引文、时间原文、提交时间、来源性质、状态和认识。一次提案中的所有修改在同一事务内校验提交。删掉聊天来源会撤销该来源和同事项依赖它的后续版本。清空历史也清除尚未生成正常聊天轮次的输入。v10 有来源的报告继续可读。
3. 回复时按发言人的统一主体检索当前认识。Pet 与配置过的主人 QQ 账号共享主体。已留存的 QQ 入站事件、Pet 经历、群聊摘要、长期摘要、激活的事实以及新增的本人报告提供跨渠道候选。查询使用当前输入、最近对话、匹配程度与时间，最多选取十二段。早期只有短期对话、没有长期留存依据的行不作历史回填。原有的会话检索工具仍可用于具体的历史查询。
4. 面对本人私聊，可以使用本人的直接证据。在群里，以及涉及其他人的证据时，单独进行内部判断。外部回复只收到程序认可的决定：照常回应、放下旧的关心、需要时确认现状。无效或超时采用保守回应。当前群本来就谈过的主题可以提示“别按旧状态追问”。当前群的记录开关控制写入；全局长期记忆启用时，已获准回复的群即使不记录，也可使用内部判断。
5. 开启自主活动后，主人私聊或 Pet 对话可以产生一个与该轮经历关联的兴趣。后台每分钟检查是否空闲、是否在安静时段、是否有到期念头。全局长期记忆开关、每日次数、冷却和额度都生效，尝试记录与下次回顾时间持久化。模型可以继续、完成或放弃，保存反思、目标、心情和实际写出的草稿。写入时核对版本，删除源轮次后迟到的回调不能复活它。
6. 私人对话中她能接续自己的念头和作品。模型明确想分享草稿，且应用仍空闲时，可投递给配置中的 Pet 或主人 QQ 私聊。按目标顺序选择一个可用目标。QQ 发送依赖已有客户端；调用发送接口仅代表已尝试发送。

## 启用和验收

跨渠道认识跟随全局长期记忆开关。要辨认跨渠道的主人，请在身份配置中登记本人的 QQ 账号。首次打开旧库时，SQLite 以事务迁移到 v17。v10 主题继续作为历史证据，已有行不重新提交给提取模型。更早版本先执行已有迁移。升级前如需保留旧版运行环境，可在应用关闭时备份记忆库。

v17 为 matter_revision 增加可空 semantics：事项进度 lifecycle、证据确定性 certainty、完成条件 completion_condition、时间适用范围 temporal_scope 和来源时间表达 time_expression 分开保存。time_anchor 由服务端关联来源报告时间；不自动把模糊日期解析为确定日期。旧行保持原样，读取时明确标记 legacy，未知进度不按“未完成”解释。前台目录同时携带完整认识及获准的初始来源，逐句审计只核查结构、引用与覆盖，不把模型自报视为语义证明。实现与验证边界见 [本轮交接](RETRIEVAL_REFACTOR_STATUS.md)。

认识更新在前台回复之外异步运行，每条符合条件的消息通常增加一次模型请求。无工作时只查询本地队列。同一主体的输入顺序处理，网络错误从 30 秒开始退避，最长一小时。协议、证据或基版本连续被拒的事件累计三次后记录为 review_required，后续事件可以继续。needs_evidence 触发一次历史补查，有可用依据才再请求一次模型。第二次仍无法判断时保存未决结果。同会话较早事件的聊天记录晚到时可重新排队。升级到 v12 时，保有输入的旧 needs_evidence 和 review_required 各重新排队一次。新增 [Cognition] 日志使用 run、来源序号、候选事项及版本、退出原因和耗时关联提案与上下文，正文仍受原有聊天调试日志设置影响。

安静回顾使用现有身份配置中的 `proactive.enabled`、`idle_seconds`、`daily_limit`、`cooldown_seconds` 和 `quiet_hours`。想让她主动展示作品，可在 `targets` 中明确加入 `{"channel":"pet","session_id":"default"}`。没有分享目标也能在后台写作，之后在私人对话中聊起作品。日历式主动问候与空闲回顾是两种不同的触发条件，本次接入空闲回顾。

验收可以沿着日常交流进行：

- 在群里说“我脚踝还疼”，再在私聊补充“脚踝已经好了”。换 Pet 直接问近况，检查是否依据更正回答。回到群里聊周末出行，观察是否停止重复提醒或引用私聊。
- 应用重启后重复测试。删除新报告的源轮次，再检查当前认识是否还依赖它。QQ 的原始事件有自己的留存和删除路径，删除聊天轮次不会自动删除独立保存的原始事件。
- 在两个渠道快速连续输入，再制造一次网络失败，检查各自的回复目标和下一轮能否继续。
- 在开启主动活动后私聊讨论一个创作点子。若产生了兴趣，经过空闲回顾后，可以询问草稿。她也可能选择暂时搁置。

自动化覆盖数据库迁移、本人更正、逐字证据、重启、删除、回顾预算与版本控制。回环 HTTP 测试使用正常 LLM 消息组装与网络解析路径，检查群聊表达与 Pet 请求均可获得相关的跨渠道证据。真实模型能否稳定提取主题与自然表达，需要实际使用观察。



## 配置、资源与验证入口

配置形状、默认值与开关范围只维护在 [配置参考](CONFIG_REFERENCE.md)。app_data 位于可执行文件目录，包含用户配置、记忆与日志。运行数据保持 Git 忽略。UI、TTS 和资源部署见 [项目 README](../README.md)，GPT-SoVITS 补充材料见 [语音参考](GPT_SOVITS_REFERENCE.md)。

构建入口为根 CMakeLists.txt，自动测试注册在 tests/CMakeLists.txt。测试的临时库与正式 app_data 应隔离。本次文档整理未执行新的构建或模型调用。真实对话是否改善需记录运行版本、输入、证据和最终回复，已有测试结果保留其发生日期。

## v12 历史依据与重算

主人 QQ 私聊启用 owner_cross_channel_private 时，回复准备阶段从 Pet 历史中读取同一主体在消息时间之前的发言。每个来源会话最多六条，按匹配度和近况选取。旧聊天行可在这一授权范围内作为临时背景。用于持久化修订的补查另需具备认知来源、旧版状态报告或念头等保存依据。群聊补查限本群当前主体。补查再次应用盲区和敏感内容规则。

observation_history 记录补查轮次实际提供的聊天行。删除其中任一行时，事务撤销依赖结果，并沿同事项的后续修订传递，相关输入重新进入队列。对没有显式来源引用的旧自由文本推断，尚无法恢复完整依赖关系。
## v13 前台交谈与历史

后续 v14 继续使用本节的输入留存、渠道交接和中断状态。

接受消息后，允许留存的输入立即写入 chat_history，获得一条 turn_state 为 received 的记录。允许后台观察时，同一事务先尝试登记认知来源。前台按接收顺序工作，读取本条输入之前的会话历史，单独传入当前消息。摘要也以本次完成的行号为上界。模型可直接接话、澄清或主动收尾。后台观察有独立的重试周期，前台可以使用当前消息里的更正。

CURRENT_UNDERSTANDING 提供事项目录、本人陈述和相关完整认识。咨询按本轮引用读取修订链并核验版本。群聊表达直接接收相关内部背景与听众信息。原始事件可经 memory_recall/read 补查。

数据库提交认识或删除依据后，程序比较已选择事项对当前听众的投影。可见依据有变化时取消旧生成，用新资料重新表达一次。网络回调也会复核，覆盖提交与回调次序互换的情况。再次变化会结束处理并记录 referenced_matter_changed。未整理的输入是否涉及某件事仍需语义判断。当前变更通知覆盖已提交的事项，短期历史、跨渠道原始资料和历史工具结果尚无同等粒度的动态订阅。

| turn_state | 含义与读取方式 |
|---|---|
| received | 已保存用户输入，尚无持久化处理结果。进程被强制终止后可能保留此状态。 |
| handed_off | Pet 的句子已入语音队列，或 QQ 消息已提交本地 WebSocket。raw_reply 保存表达内容。播放完成和远端送达仍需独立回执。 |
| quiet | 模型显式选择 close 或 defer 并返回空句。turn_detail 保存决定，历史只展示用户发言。带正文的 close 按正常表达保存。 |
| interrupted | 网络、输出协议、持续变化的已选依据、渠道交接失败或正常退出终止了处理。turn_detail 保存原因，历史只展示用户发言。 |
| legacy | v12 及以前的记录保留原文和来源。历史空句的表达原因未知。 |

前台完成只能更新同一条 received 记录。交谈阶段按渠道、会话和主体读取，主动决定及已交接的正文可推进阶段。技术中断不会产生角色发言或沉默决定。设置窗口分别显示安静决定、中断原因和旧空行。重启后不会自动重发未确认交接的请求。

## v14 证据与回答契约

事项目录携带当前状态与报告时间。前台按输入提取检索词，最多为四个匹配事项内联完整认识。其余事项通过 consult 核对。词匹配用于上下文预算选择，语义关联与证据充分性由模型判断。内联事项与显式核对事项共同参与生成期间的变更检查。

认识保留 quote、source_kind、reported_at、event_time_expression 和 interpretation_recorded_at，并提供 source/N 编号。effective_at 无法证明时为 null。发言时间、事件时间和认识生效时间分别解释，事件时间原文继续保留。consult 最多返回每事项八条可见修订，群聊原文链限定本群。短期用户发言在正文中注明发言时间。旧助手回复只证明曾经发生过该表达。

前台 evidence_decision 报告 question、question_type、used_evidence、evidence_status、basis、unresolved_conflicts 和 consult_required。具体事实可由明确的直接证据支持，最新决定需关注后续修订，范围概览需覆盖，未命中不足以支持不存在断言。partial 表示检索覆盖、时间精度或截断等不完全；需结合逐项状态解释。basis 只保存简短证据依据。

cognitive_audit 将决策标记为 model_reported 或 missing，并记录来源编号检查结果。编号检查通过表明引用来自提供的资料或已核对事项，语义充分性仍属模型报告。未知编号记录 unknown_references 和 unverified。模型省略审计字段时记录 missing，避免补造理由。

自 2026-11-11 起，程序在组装资料和工具结果时登记可引用的编号。回答审计里的 provided_evidence 与 cited_evidence 分别保存本轮提供和模型引用的资料范围。目录编号可支持其展示的标题、状态和报告时间。完整认识包含当前依据，consult 返回受听众范围约束的修订记录。会话中用户的发言与茉子先前的表达有独立编号。旧审计保留其生成时的校验结果。

前台按当前输入和最近一次用户发言与事项的词项匹配度选择最多四个完整认识。私聊和 Pet 自动读取近期群消息需要近期话题线索，定向查询为空时，明确提及群的私聊请求可以回退群消息概览。已安装默认提示中的旧必答条款在读取时适配，主动安静由交谈动作协议定义。

认识更新遭遇 base_mismatch 时，observation_attempt 的 base_conflict 保存变更序号、事项、预期版本、实际版本和失败条件。失败条件区分事项缺失、归属不同、版本不一致和来源顺序冲突。

后台区分 missing_choice、missing_content、empty_completed、response_truncated、invalid_json、网络错误及提案校验错误。响应失败和基版本变化进入退避重试，每次重新读取来源与基版本。提案合法性拒绝累计后进入 review_required。观察审计记录 finish_reason、usage、基版本与耗时。v14 迁移先记录旧 empty_content 的失败次数、错误及原结果，再恢复因空响应被搁置且仍有输入的事件。

审计遵守敏感与留存开关。删除聊天记录清除其回答审计，删除来源清除其处理审计。当前仍需完善规范化事件区间、认识生效时间推导、派生摘要的完整逐条来源映射，以及模型决策依据的稳定性。
