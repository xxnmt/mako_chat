# 架构与当前实现

> 更新：2026-11-09。源码核对基准 54a0be1。本文负责现状，未来变更只维护在 [开发路线图](ROADMAP.md)。入口见 [文档索引](README.md)。

## 组件与职责

| 模块 | 当前职责 | 源码入口 |
|---|---|---|
| 应用组装 | 创建服务、连接信号、前台排队、投递和空闲回顾检查 | appcontroller.h/.cpp、main.cpp |
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

渠道输入经 SessionRouter 生成 RequestMeta 后，AppController 将请求排入前台队列。实际启动时重读当前会话历史，再调用 LLMService。LLMService 先准备跨渠道证据，必要时做私下判断，再组装最终上下文并进入聊天及记忆工具循环。前台结果携带对应输入和元数据，AppController 保存本轮、投递到原渠道，并按阈值触发后台记忆提取。

LLMService 仍保留单份活动请求状态，当前应用依靠前台串行队列保证其调用边界。成功结果携带上下文不等于服务已支持任意并发调用。MemoryAgent 的结果及失败带请求 ID；主网络回复校验 runId。原生工具拒绝时可回退 JSON Envelope，输出异常与空回复执行有限重试。

Pet 走语音合成和音频 sink，QQ 走文本发送。QQ 情绪可按配置影响立绘。播放收尾通过轮次序号约束退火，昼夜与退火属于表现管理。

## 记忆与持久化

当前 SQLite user_version 为 10。基础表包括 chat_history、character_profile、episodic_memory、long_term_summary、relationship_state；多渠道之后增加 raw_events、memory_cursor、interactions、人物身份、claims 和群成员关系。完整字段以 MemoryManager::initDatabase 为准，v10 在 chat_history 中增加 state_updates、thought、thought_version、thought_result、review_after，并增加 thought_review_log 与索引。

会话短期历史按 channel/session_id 分桶。稳定档案和关系可内联；tool 模式下历史事实主要按需检索。跨渠道 personalEvidence 与 currentUnderstanding 另行接入最终请求前的上下文准备。原始事件、聊天轮次和派生证据有各自删除边界，删除聊天记录不会自动删除独立 raw_events。

既有 R3 披露工具契约见 [R3 基线](MEMORY_V5_R3_BASELINE.md)，工具 JSON、状态和时间框架见 [检索接口](MEMORY_RECALL_TOOL.md)。R3 文档中的 v9 快照只描述当时路径，新增内部证据路径不能套用旧矩阵推断其全部行为。MemoryAccessState 定义四个枚举，当前 evaluate 实际返回 Shareable 或 Forbidden；KnownOnly/OwnerCandidate 不能据枚举定义视为完整实现。

## 实际验证与当前限制

2026-11-08 本机启动日志确认记忆库从 v9 升至 v10、QQ 连接和成员快照成功。当天实际对话中，QQ 私聊的用餐安排曾被 Pet 正确召回；同一安排在 Pet 中被描述为可能改期后，QQ 仍回答原安排，追问调整也未找到更正。群聊曾出现 memory_recall 参数错误及零结果，最终回复把检索不足说成没有记住。因此当前版本应标为实验性连续性实现，尚未达到跨渠道认知一致的验收标准。

最近一次开发验证中，Windows Debug 构建曾成功，15 个自动测试套件通过。后续召回排序修改也通过全部套件；运行中的原名可执行文件会阻止覆盖链接。测试覆盖的是受控输入下的数据与请求行为，不能据此宣称真实模型已能稳定理解更正。下述已实现路径表示源码已接入。

现有局限包括：state_updates 随回复可选输出，逐字引文校验无法保证语义正确或每次都有更新；相同主题依赖字符串名称，不能可靠表达同一事件的暂定、取消、恢复与时间变更；候选最多十二段，主题相关性和本人时间线排序可能遗漏必要证据；私下判断向表达阶段只传有限决定；回顾状态附着于源聊天行，尚无统一事件账本、独立事项模型和完整认知循环。

当前排序同时保留主题相关性与本人时间线的候选机会，未针对私聊设置固定的一天窗口。时间字段包含消息时间、事件时间和摘要整理时间，现有聚合候选尚未完整区分这些语义，这是后续重构需要处理的缺口。

这份设计从一个具体行为出发。主人在群里说过脚踝疼，后来私聊说好了。回到 Pet，她应该能接着聊康复后的生活。回到群里，她也应当放下旧的担心，同时顾及谁在听。下面区分已经接入程序的行为和后续能力。

## 已实现的运行路径

1. Pet 和 QQ 经 SessionRouter 识别人。每次有效输入生成事件 ID、运行 ID、接收时间和发生时间。AppController 顺序处理前台请求，实际开始处理时读取当前会话的历史。重试和记忆工具属于同一运行，成功结果带回原输入和渠道元数据。
2. 已允许长期保存的对话，可以随正常回复产生最多四条本人状态更新。每条包含主题和逐字证据。模型负责理解语义，程序检查长度、主体、保存资格、敏感标记和引文是否确实在本轮用户输入中。状态与来源对话写在同一 SQLite 行内。主题相同的最新报告成为当前认识，旧记录仍是历史。删除来源轮次会同时删除它的状态证据。
3. 回复时按发言人的统一主体检索当前认识。Pet 与配置过的主人 QQ 账号共享主体。已留存的 QQ 入站事件、Pet 经历、群聊摘要、长期摘要、激活的事实以及新增的本人报告提供跨渠道候选。查询使用当前输入、最近对话、匹配程度与时间，最多选取十二段。早期只有短期对话、没有长期留存依据的行不作历史回填。原有的会话检索工具仍可用于具体的历史查询。
4. 面对本人私聊，可以使用本人的直接证据。在群里，以及涉及其他人的证据时，单独进行内部判断。外部回复只收到程序认可的决定：照常回应、放下旧的关心、需要时确认现状。无效或超时采用保守回应。当前群本来就谈过的主题可以提示“别按旧状态追问”。当前群的记录开关控制写入；全局长期记忆启用时，已获准回复的群即使不记录，也可使用内部判断。
5. 开启自主活动后，主人私聊或 Pet 对话可以产生一个与该轮经历关联的兴趣。后台每分钟检查是否空闲、是否在安静时段、是否有到期念头。全局长期记忆开关、每日次数、冷却和额度都生效，尝试记录与下次回顾时间持久化。模型可以继续、完成或放弃，保存反思、目标、心情和实际写出的草稿。写入时核对版本，删除源轮次后迟到的回调不能复活它。
6. 私人对话中她能接续自己的念头和作品。模型明确想分享草稿，且应用仍空闲时，可投递给配置中的 Pet 或主人 QQ 私聊。按目标顺序选择一个可用目标。QQ 发送依赖已有客户端；调用发送接口仅代表已尝试发送。

## 启用和验收

跨渠道认识跟随全局长期记忆开关。要辨认跨渠道的主人，请在身份配置中登记本人的 QQ 账号。首次打开旧库时，SQLite 在一个迁移事务内从 v9 升至 v10；不会再次向模型发送全库进行重建。更早版本先执行已有迁移。升级前如需保留旧版运行环境，可在应用关闭时备份记忆库。

安静回顾使用现有身份配置中的 `proactive.enabled`、`idle_seconds`、`daily_limit`、`cooldown_seconds` 和 `quiet_hours`。想让她主动展示作品，可在 `targets` 中明确加入 `{"channel":"pet","session_id":"default"}`。没有分享目标也能在后台写作，之后在私人对话中聊起作品。日历式主动问候与空闲回顾是两种不同的触发条件，本次接入空闲回顾。

验收可以沿着日常交流进行：

- 在群里说“我脚踝还疼”，再在私聊补充“脚踝已经好了”。换 Pet 直接问近况，检查是否依据更正回答。回到群里聊周末出行，观察是否停止重复提醒或引用私聊。
- 应用重启后重复测试。删除新报告的源轮次，再检查当前认识是否还依赖它。QQ 的原始事件有自己的留存和删除路径，删除聊天轮次不会自动删除独立保存的原始事件。
- 在两个渠道快速连续输入，再制造一次网络失败，检查各自的回复目标和下一轮能否继续。
- 在开启主动活动后私聊讨论一个创作点子。若产生了兴趣，经过空闲回顾后，可以询问草稿。她也可能选择暂时搁置。

自动化覆盖数据库迁移、本人更正、逐字证据、重启、删除、回顾预算与版本控制。回环 HTTP 测试使用正常 LLM 消息组装与网络解析路径，检查群聊私下判断能看到证据、公开回复上下文没有私聊原句、Pet 可以使用该证据。真实模型能否稳定提取主题与自然表达，需要实际使用观察。



## 配置、资源与验证入口

配置形状、默认值与开关范围只维护在 [配置参考](CONFIG_REFERENCE.md)。app_data 位于可执行文件目录，包含用户配置、记忆与日志。运行数据保持 Git 忽略。UI、TTS 和资源部署见 [项目 README](../README.md)，GPT-SoVITS 补充材料见 [语音参考](GPT_SOVITS_REFERENCE.md)。

构建入口为根 CMakeLists.txt，自动测试注册在 tests/CMakeLists.txt。测试的临时库与正式 app_data 应隔离。本次文档整理未执行新的构建或模型调用。真实对话是否改善需记录运行版本、输入、证据和最终回复，已有测试结果保留其发生日期。
