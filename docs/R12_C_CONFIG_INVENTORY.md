# R12-C1 配置生命周期清单

按基线 111 项界面顺序。配置文件为所在应用目录下 app_data/config 中的文件。运行入口位置相对 src。默认和控件约束保留基线值，C1 名称及可见性变化见实施报告。条件及边界说明该值是否参与运行。

### DeepSeek

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| API 密钥 / `api.deepseek_api_key` | setting.json | `""` / `secret` | AppController 设置回调（appcontroller.cpp:845）→ LLMService::setApiKey | 新请求 | 模型鉴权，空密钥不提供有效服务 |

### 阿里云百炼

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| API 密钥 / `api.dashscope_api_key` | setting.json | `""` / `secret` | ImagePerception::observe（cognition/imageperception.cpp:50–105）、LLMService::retrieveMemory（cognition/llmservice.cpp:379–393）、MemoryIndexWorker::run（memory/memoryindexworker.cpp:105–114） | 新请求或索引批次 | 视觉及检索鉴权，显式密钥优先于向量环境变量 |
| 服务地址 / `api.dashscope_base_url` | setting.json | `""` / `text` | ImagePerception::observe（cognition/imageperception.cpp:50–105）、LLMService::retrieveMemory（cognition/llmservice.cpp:379–393）、MemoryIndexWorker::run（memory/memoryindexworker.cpp:105–114） | 新请求或索引批次 | 视觉基地址及检索端点派生，显式检索端点优先 |
| 识别对话图片 / `api.vision_enabled` | setting.json | `false` / `bool` | ImagePerception::observe（cognition/imageperception.cpp:50–58） | 下一次图片感知 | 仅影响有图且获准的回复 |
| 视觉模型 / `api.vision_model` | setting.json | `"qwen3-vl-plus"` / `text` | ImagePerception::observe（cognition/imageperception.cpp:50–58） | 下一次图片感知 | 视觉模型，空值使图片感知不可用 |

### 播放与服务

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 语音方式 / `tts.mode` | setting.json | `"api"` / `choice:api=GPT-SoVITS\|mock=模拟播放` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | API 或模拟播放 |
| API 地址 / `api.gpt_sovits_url` | setting.json | `"http://127.0.0.1:9880"` / `text` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | API 服务端点 |
| Python 可执行文件 / `tts.python_path` | setting.json | `""` / `file` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | 本地服务解释器 |
| GPT-SoVITS 安装目录 / `tts.gpt_sovits_root_path` | setting.json | `""` / `directory` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | 本地语音服务安装根目录 |

### 参考音频

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 参考音频文件夹 / `tts.reference_audio_dir` | setting.json | `""` / `reference_directory` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | 空值用 app_data/reference_audio，从映射选择素材 |

### 模型文件

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| GPT 权重 / `tts.gpt_weights_path` | setting.json | `""` / `file` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | GPT 权重切换 |
| SoVITS 权重 / `tts.sovits_weights_path` | setting.json | `""` / `file` | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | SoVITS 权重切换 |
| GPT 模型目录 / `tts.gpt_model_dir` | setting.json | `""` / `directory` | ConfigManager（configmanager.cpp:620–637），无运行调用 | 隐藏、兼容读取 | 仅旧模型目录值 |
| SoVITS 模型目录 / `tts.sovits_model_dir` | setting.json | `""` / `directory` | ConfigManager（configmanager.cpp:620–637），无运行调用 | 隐藏、兼容读取 | 仅旧模型目录值 |

### 对话记忆

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 连续输入等待（毫秒） / `conversation.input_pause_ms` | setting.json | `1600` / `num:0:6000` | CognitiveCoordinator::submit（cognitivecoordinator.cpp:293） | 下一条输入 | 0 关闭合并，批次最长等待仍由协调器控制 |
| 使用近期对话 / `memory.short_context_enabled` | setting.json | `true` / `bool` | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 短期收录和窗口开关 |
| 近期对话轮数 / `memory.short_term_length` | setting.json | `15` / `num:0:999` | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 近期轮数 |
| 使用长期记忆 / `memory.long_term_memory_enabled` | setting.json | `true` / `bool` | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 长期记忆与后台认知门控 |
| 摘要间隔（轮） / `memory.summary_length` | setting.json | `15` / `num:1:999` | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 摘要调度阈值 |

### 检索和索引

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用本地检索索引 / `memory.retrieval.index_enabled` | setting.json | `true` / `bool` | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 索引维护和索引召回开关 |
| 自动回填空索引 / `memory.retrieval.auto_backfill` | setting.json | `false` / `bool` | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 只回填空索引 |
| 候选数量上限 / `memory.retrieval.candidate_limit` | setting.json | `40` / `num:10:200` | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 候选和重排输入上限 |
| 启用重排 / `memory.retrieval.rerank_enabled` | setting.json | `true` / `bool` | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 远程重排开关，关闭仍本地排序 |
| 检索结果上限 / `memory.retrieval.rerank_limit` | setting.json | `10` / `num:1:40` | MemoryRetrievalService，重排与本地截断（memory/memoryretrieval.cpp:588–636） | 下一查询或安全批次 | 最终结果上限，关闭重排仍裁剪 |

### 主动回忆工具

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用补查工具 / `memory.agent_enabled` | setting.json | `true` / `bool` | MemoryAgentTools::canExposeTools | 下一轮 | 主模型补查权限 |
| 回忆方式 / `memory.agent_mode` | setting.json | `"tool"` / `choice:tool=模型工具\|legacy=独立规划` | ConfigManager::loadSetting | 下一轮 | 历史键。legacy 加载时记录兼容诊断，使用主模型工具 |
| 工具协议 / `memory.agent_protocol` | setting.json | `"native"` / `choice:native=原生调用\|json_envelope=JSON 信封` | LLMService::startMemoryToolPath | 下一轮 | 原生或 JSON 信封 |
| 定向检索预算（毫秒） / `memory.agent_timeout_ms` | setting.json | `6000` / `num:1000:15000` | MemoryRecallTool::execute → MemoryExecutor | 下一轮 | 定向执行预算，概览独立 |
| 最多工具轮数 / `memory.agent_max_rounds` | setting.json | `2` / `num:1:4` | LLMService 工具回调 | 下一轮 | 补查轮数另受共享查询预算限制 |
| 最多返回条数 / `memory.agent_max_results` | setting.json | `8` / `num:1:20` | LLMService 构造 MemoryAgentScope → MemoryRecallTool | 下一轮 | 共享结果上限 |

### 连接

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用 QQ 渠道 / `qq.enabled` | setting.json | `false` / `bool` | AppController 差异应用（appcontroller.cpp:845）→ QQProtocolClient | 变化时重连 | QQ 渠道开关，不删除历史 |
| WebSocket 地址 / `qq.ws_url` | setting.json | `"ws://127.0.0.1:3001"` / `text` | AppController 差异应用（appcontroller.cpp:845）→ QQProtocolClient | 变化时重连 | 连接地址 |
| 访问令牌 / `qq.access_token` | setting.json | `""` / `secret` | AppController 差异应用（appcontroller.cpp:845）→ QQProtocolClient | 变化时重连 | 传输鉴权 |

### 回复范围

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 私聊回应范围 / `qq.reply.private` | setting.json | `"master"` / `choice:none=关闭\|master=仅主人\|allowlist=允许列表\|all=全部` | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 私聊回复域 |
| 群聊回应方式 / `qq.reply.group` | setting.json | `"none"` / `choice:none=关闭\|mention=仅被 @ 时\|all=全部` | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 群回复模式与 @ 触发 |
| 主人可在任意群触发 / `qq.reply.master_any_group` | setting.json | `false` / `bool` | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 主人群例外 |
| 空名单也限制群回复 / `qq.reply.restrict_empty_group_list` | setting.json | `false` / `bool` | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 是否限制空回复名单 |
| 群权限 / `qq.group_permissions` | setting.json | `QJsonObject{}` / `group_permissions` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 虚拟表格映射群记录和回复名单，检索另核权 |
| 私聊账号权限 / `qq.private_permissions` | setting.json | `QJsonObject{}` / `private_permissions` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 虚拟表格映射私聊记录和回复名单，检索另核权 |

### 群聊记忆

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用群聊记忆 / `qq.group_memory_enabled` | setting.json | `false` / `bool` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 群记录总开关 |
| 保留天数（清理暂未接入） / `qq.group_retention_days` | setting.json | `30` / `num:1:3650` | ConfigManager::loadSetting/saveSetting（config/configmanager.cpp:291、457），无清理消费者 | 兼容读写，无运行清理 | 隐藏，兼容读写，无到期清理 |
| 成员刷新间隔（分钟） / `qq.membership_refresh_minutes` | setting.json | `30` / `num:0:1440` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 0 停止周期刷新 |
| 成员缓存有效期（分钟） / `qq.membership_max_age_minutes` | setting.json | `1440` / `num:1:10080` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 成员证据缓存有效期 |
| 各群记忆模式 / `qq.group_memory_mode` | setting.json | `QJsonObject{}` / `modes` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 逐群覆盖记录模式 |
| 各群发送者允许列表 / `qq.group_sender_allowlist` | setting.json | `QJsonObject{}` / `senders` | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | strict 模式的发送者名单 |

### 群消息整理

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 空闲后整理（分钟） / `memory.qq_group_idle_minutes` | setting.json | `15` / `num:1:1440` | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 空闲触发整理分钟数 |
| 开始整理的消息数 / `memory.qq_group_batch_size` | setting.json | `50` / `num:1:500` | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 触发整理并限制批读取条数 |
| 分段间隔（秒） / `memory.interaction_gap_seconds` | setting.json | `90` / `num:10:3600` | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 消息间隔超过后分段 |
| 每段最多消息数 / `memory.interaction_max_events` | setting.json | `20` / `num:1:100` | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 段内事件数量 |
| 每段最长时间（分钟） / `memory.interaction_max_minutes` | setting.json | `10` / `num:1:120` | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 段内最长分钟数 |

### 近期群聊上下文

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 使用近期群聊上下文 / `memory.recent_group_context.enabled` | setting.json | `true` / `bool` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 近期群窗口开关 |
| 回看范围（分钟） / `memory.recent_group_context.max_age_minutes` | setting.json | `30` / `num:1:1440` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 回看时间窗 |
| 每群候选上限 / `memory.recent_group_context.candidate_limit_per_group` | setting.json | `50` / `num:1:500` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 每群候选 |
| 最多参考群数 / `memory.recent_group_context.max_owner_groups` | setting.json | `2` / `num:1:20` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 主人相关群数量 |
| 定向查找条数 / `memory.recent_group_context.targeted_item_limit` | setting.json | `10` / `num:1:100` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 定向条数 |
| 定向查找字数 / `memory.recent_group_context.targeted_char_budget` | setting.json | `1200` / `num:100:8000` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 定向字数 |
| 概览条数 / `memory.recent_group_context.overview_item_limit` | setting.json | `50` / `num:1:500` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 概览条数 |
| 概览字数 / `memory.recent_group_context.overview_char_budget` | setting.json | `2200` / `num:100:12000` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 概览字数 |
| 近期窗口条数 / `memory.recent_group_context.hot_window_items` | setting.json | `10` / `num:1:100` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 保留完整原文的近期条数 |
| 较旧单条消息字数 / `memory.recent_group_context.warm_line_max_chars` | setting.json | `24` / `num:8:200` | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 较旧单条概览字数 |

### 记忆权限

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 允许主人跨私聊渠道使用个人记忆 / `memory.owner_cross_channel_private` | setting.json | `false` / `bool` | ActionPolicy::visibleSession、MemoryPolicy（memorypolicy.cpp:35）、AppController（appcontroller.cpp:228、257） | 下一轮，权限缩减对账 | 跨私聊个人记忆读取，对外披露仍复核 |
| 策略版本标识 / `memory.privacy_policy_version` | setting.json | `"r3"` / `text` | ActionPolicy::visibleSession、MemoryPolicy（memorypolicy.cpp:35）、AppController（appcontroller.cpp:228、257） | 下一轮，权限缩减对账 | 隐藏，仍写入原始事件作审计 |
| 内容过滤规则 / `memory.privacy_rules` | setting.json | `QJsonArray{}` / `rules` | ActionPolicy::visibleSession、MemoryPolicy（memorypolicy.cpp:35）、AppController（appcontroller.cpp:228、257） | 下一轮，权限缩减对账 | exact 为字面子串，pattern 为正则 |

### 身份与会话

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 茉子的 QQ 账号 / `identity.self.qq` | identity.json | `QJsonArray{}` / `ids` | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 自身回声过滤 |
| 主人的 QQ 账号 / `identity.master.qq` | identity.json | `QJsonArray{}` / `ids` | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 主人身份与私域授权 |
| 记忆盲区 / `memory.blind_zones` | identity.json | `QJsonArray{}` / `zones` | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 会话隐私边界 |

### 桌宠表现

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 桌宠跟随外部渠道情绪 / `appearance.follow_external_mood` | identity.json | `true` / `bool` | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 控制外部渠道情绪是否影响桌宠 |

### 主动联系与回顾

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 允许主动联系 / `proactive.enabled` | identity.json | `false` / `bool` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 主动外发授权 |
| 允许私下回顾与创作 / `proactive.private_writing` | identity.json | `false` / `bool` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 控制新源回顾和私下活动，Concern 和 OpenWake 独立 |
| 关注再检查间隔（分钟） / `proactive.concern_default_check_minutes` | identity.json | `720` / `num:0:10080` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 默认复查分钟数，0 等新依据 |
| 关注定时复查保留期（天） / `proactive.concern_retention_days` | identity.json | `14` / `num:1:365` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 定时复查和失败重试窗，不删历史 |
| 回顾间隔（秒） / `proactive.cooldown_seconds` | identity.json | `900` / `num:60:604800` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 共享回顾间隔至少60秒 |
| 空闲等待（秒） / `proactive.idle_seconds` | identity.json | `1800` / `num:0:604800` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 空闲门槛，0 不额外等待 |
| 安静开始 / `proactive.quiet_hours.start` | identity.json | `"23:00"` / `time` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 安静起点 |
| 安静结束 / `proactive.quiet_hours.end` | identity.json | `"08:00"` / `time` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 安静终点，起止相同无安静窗口 |
| 允许联系的会话 / `proactive.targets` | identity.json | `QJsonArray{}` / `targets` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 主动联系目标仍核权 |

### 每日回顾额度与主动联系上限

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 每日回顾上限 / `proactive.daily_limit` | identity.json | `3` / `num:0:999` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 共享回顾预算，0 停止领取 |
| 每日自主唤醒上限 / `cognition.open_wake.max_per_day` | identity.json | `12` / `num:0:96` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 0 不领取，正值受共享预算 |
| 每日主动联系上限（0 表示不限） / `proactive.contact_daily_limit` | identity.json | `3` / `num:0:99` | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 主动投递上限，0 不限 |

### 内部唤醒与快照

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用内部唤醒（OpenWake） / `cognition.open_wake.enabled` | identity.json | `false` / `bool` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 启用内部调度 |
| 允许内部唤醒联系主人 / `cognition.open_wake.allow_contact` | identity.json | `false` / `bool` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 联系须额外满足主动权限和目标 |
| 共享每日预算占比（%） / `cognition.open_wake.shared_budget_percent` | identity.json | `30` / `num:1:100` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 共享占比并受共享总限 |
| 认知快照提示词预算（字节） / `cognition.snapshot_budget_bytes` | identity.json | `4096` / `num:2048:8192` | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 仅快照 JSON 大小 |
| 启用统一认知上下文（B5 实验功能） / `cognition.context_actual_enabled` | identity.json | `false` / `bool` | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 实验，setter > 环境变量 > 配置 |

### 高级设置：调度与失败退避

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 首次唤醒延迟（秒） / `cognition.open_wake.initial_delay_seconds` | identity.json | `900` / `num:60:86400` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 仅初始化无有效计划时的延迟 |
| 默认唤醒间隔（秒） / `cognition.open_wake.default_interval_seconds` | identity.json | `1800` / `num:300:86400` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 次次模型未给时间时默认值 |
| 最小唤醒间隔（秒） / `cognition.open_wake.min_interval_seconds` | identity.json | `300` / `num:300:86400` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 后续安排最小间隔 |
| 最大唤醒间隔（秒） / `cognition.open_wake.max_interval_seconds` | identity.json | `86400` / `num:300:86400` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 后续安排最大间隔 |
| 单次唤醒最多模型步数 / `cognition.open_wake.max_model_steps_per_wake` | identity.json | `3` / `num:1:8` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 诊断值无执行门控，隐藏 |
| 到期后先让出的普通回顾次数 / `cognition.open_wake.max_non_open_jobs_when_due` | identity.json | `3` / `num:0:20` | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 到期后允许先让出的普通任务数 |
| 回顾失败退避（秒，逗号分隔） / `cognition.review_backoff_seconds` | identity.json | `QJsonArray{120, 300, 900}` / `ints` | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 持久化每来源退避序列 |
| 连续失败暂停阈值 / `cognition.max_review_failures` | identity.json | `4` / `num:1:10` | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 达到次数后暂停 |

### 检索服务（高级诊断）

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用向量召回 / `memory.retrieval.embedding_enabled` | setting.json | `false` / `bool` | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 关闭向量仍可词面匹配 |
| 向量服务 / `memory.retrieval.embedding_provider` | setting.json | `""` / `choice:=未指定\|dashscope=阿里云百炼\|local_hash_test=本地测试` | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | DashScope 或已有诊断 hash |
| 向量端点 / `memory.retrieval.embedding_endpoint` | setting.json | `""` / `text` | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 显式端点优先 |
| 向量密钥环境变量 / `memory.retrieval.embedding_api_key_env` | setting.json | `"DASHSCOPE_API_KEY"` / `text` | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 配置密钥优先于向量环境变量 |
| 向量模型 / `memory.retrieval.embedding_model` | setting.json | `""` / `text` | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 模型身份须匹配向量索引 |
| 向量维度（0 为自动） / `memory.retrieval.embedding_dimension` | setting.json | `0` / `num:0:8192` | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 0 对应 DashScope 1024、本地 hash 64 |
| 重排端点 / `memory.retrieval.rerank_endpoint` | setting.json | `""` / `text` | LLMService::retrieveMemory（cognition/llmservice.cpp:391–400）、工具检索（memory/memoryagent.cpp:56–60） | 下一查询 | 显式端点优先且须开启重排 |
| 重排模型 / `memory.retrieval.rerank_model` | setting.json | `"qwen3-rerank"` / `text` | LLMService::retrieveMemory（cognition/llmservice.cpp:391–400）、工具检索（memory/memoryagent.cpp:56–60） | 下一查询 | 仅实际请求时应用 |

### 已退役的独立规划设置（保留 C1 索引）

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 启用第二版检索规划 / `memory.agent_plan_v2` | setting.json | `true` / `bool` | 无运行消费者 | 下一轮 | 历史键，保留原值，退出设置界面 |
| 规划触发方式 / `memory.agent_trigger` | setting.json | `"memory_question"` / `choice:memory_question=回忆类问题\|always=每轮` | 无运行消费者 | 下一轮 | 历史键，保留原值，退出设置界面 |

### 日志与诊断

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 日志文件夹 / `logging.directory` | setting.json | `""` / `log_directory` | RuntimeLog::configure 与 AppController（appcontroller.cpp:845）、ConfigManager::loadSetting（configmanager.cpp:239） | 局部立即应用 | 空值使用 app_data/logs |
| 每次启动单独创建日志 / `logging.separate_runs` | setting.json | `true` / `bool` | RuntimeLog::configure 与 AppController（appcontroller.cpp:845）、ConfigManager::loadSetting（configmanager.cpp:239） | 局部立即应用 | 按启动独立或按日追加 |
| 记录完整对话与模型请求 / `memory.debug_full_logs` | setting.json | `false` / `bool` | ConfigManager::loadSetting（config/configmanager.cpp:240）发布日志开关；LLMService（cognition/llmservice.cpp:1751、4340、4425 等）、Pet/QQ 适配器和 ChatWidget 在产生日志时读取 | 局部立即应用 | 诊断正文及请求日志 |

### 昼夜切换

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 夜间开始 / `time.night_start` | setting.json | `"22:00"` / `time` | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 夜间开始 |
| 夜间结束 / `time.night_end` | setting.json | `"07:00"` / `time` | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 白天开始 |

### 状态持续时间

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 脸红（秒） / `time.blush_lasting` | setting.json | `30` / `num:1:3600` | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 脸红持续秒数 |
| 表情（秒） / `time.emotion_lasting` | setting.json | `15` / `num:1:3600` | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 表情持续秒数 |
| 距离（秒） / `time.distance_lasting` | setting.json | `15` / `num:1:3600` | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 距离状态持续秒数 |

### 角色提示词

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| 角色提示词 / `prompt` | prompt.txt | `""` / `multiline` | LLMService::loadSystemPrompt | 下次请求 | 读取 prompt.txt，保存校验非空 |


## 隐藏、历史及覆盖入口

|位置 / 类型|运行消费者及生效|处置|
|---|---|---|
|setting.json / memory.retrieval.legacy_fallback|当前生产解析器与检索没有该开关，旧文档可能存在|未实现于当前路径，保留未知键|
|identity.json / proactive.schedule，数组项 at/type|原有解析只有缓存和设置回读，没有调度消费者。真实提醒走 reminder 执行链|移除无效解析，旧原始键由原子合并保存保留|
|setting.json / tts.prompt_lang、tts.prompt_text、tts.ref_audio_path|ConfigManager 不读取这些顶层 TTS 字段。语音请求里的同名字段由 ApiTTSProvider 根据 TTSReferenceManager 对 tts_map.json 的条目生成（apittsprovider.cpp:43、ttsreferencemanager.cpp:24）|保留旧键和当前协议，普通 UI 继续只编辑参考目录|
|setting.json / qq.group_memory_allow、qq.reply.group_allow、private_allow、private_memory_allow|权限表格真实持久化键，ConfigManager（276–290）及 MemoryPolicy（156–206）按记录或回应使用。private_memory_allow 缺省继承旧回复名单，显式空数组与缺省含义不同|保留，虚拟 UI 字段不写入 JSON|
|identity.json / cognition.semantic_verifier.enabled、max_verifier_calls、max_repairs|ConfigManager 解析默认 true/1/1，数值夹紧 0..4 / 0..2。LLMService 的新行动核验使用 enabled 和 maxVerifierCalls 是否至少为 1，实际每行动最多一次。maxRepairs 存量值没有新行动 repair 消费者|安全配置仅保留内部入口，历史 repair 值不作为有效能力展示|
|identity.json / proactive.targets[].channel/session_id、memory.blind_zones[].channel/session_id|ConfigManager（892–899、985–991），渠道和会话粒度。ActionPolicy、MemoryReadPolicy 与出站 guard 对目标及来源分别查权限|保留|
|setting.json / memory.privacy_rules[].id/type/value/action|ConfigManager（206–230）；SensitiveContentFilter 在收录/查询/表达阶段消费。类型不合法或值为空跳过。策略版本记录审计元数据|保留用户规则，策略版本退出普通编辑|
|MAKO_COGNITIVE_CONTEXT_ACTUAL / 明确 setter|LLMService::cognitiveContextActualEnabled，测试 setter 优先，非空环境覆盖配置，按下一轮读取|实验覆盖|
|MAKO_COGNITIVE_SHADOW / 明确 setter|LLMService::cognitiveShadowEnabled，环境首次读取缓存，setter 可覆盖|诊断，不增加一个模型请求|
|DASHSCOPE_API_KEY 或 embedding_api_key_env 指定环境变量|MemoryIndexWorker、MemoryAgent 与 LLMService 异步检索，配置密钥优先|鉴权回退，不在 UI 回显实际密钥|
|local_hash_test、MAKO_SETTINGS_PREVIEW、MAKO_R12_ACCEPTANCE_KEY/URL|本地索引与评测可使用 hash；其余只在设置预览或隔离验收测试读取|诊断和测试，普通 provider 选项不提供新选取|
|默认/样例配置|仓库未跟踪可直接部署的 setting.json 或 identity.json。setting.json 缺失初始化及默认序列化在 ConfigManager，identity.json 缺失按结构体和解析默认；bench、测试和构建目录的 app_data 不作为仓库默认|用户文件不迁移、不重写|

文件保存仍使用 QSaveFile；有效 JSON 对象之外、非法设置类型及身份冲突返回明确错误或按现有解析归一。时间编辑保留秒数。表中“保存后下一轮”描述配置读取时机，已经启动的模型请求保留原上下文。
