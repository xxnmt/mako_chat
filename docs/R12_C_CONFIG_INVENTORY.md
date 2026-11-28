# R12-C1 配置生命周期清单

按基线 111 项界面顺序。配置文件为所在应用目录下 app_data/config 中的文件。运行入口位置相对 src。默认和控件约束保留基线值，C1 名称及可见性变化见实施报告。条件及边界说明该值是否参与运行。

### DeepSeek

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `""` / `secret` | setting.json | 689 / 通用或未单列 | AppController 设置回调（appcontroller.cpp:845）→ LLMService::setApiKey | 新请求 | 模型鉴权，空密钥不提供有效服务 |

### 阿里云百炼

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `""` / `secret` | setting.json | 692 / 通用或未单列 | ImagePerception::observe（imageperception.cpp:50）；LLMService::retrieveMemory（llmservice.cpp:379） | 新请求或索引批次 | 视觉及检索鉴权，显式密钥优先于向量环境变量 |
| `""` / `text` | setting.json | 693 / 通用或未单列 | ImagePerception::observe（imageperception.cpp:50）；LLMService::retrieveMemory（llmservice.cpp:379） | 新请求或索引批次 | 视觉基地址及检索端点派生，显式检索端点优先 |
| `false` / `bool` | setting.json | 694 / 100 | ImagePerception::observe（imageperception.cpp:50）；LLMService::retrieveMemory（llmservice.cpp:379） | 新请求或索引批次 | 仅影响有图且获准的回复 |
| `"qwen3-vl-plus"` / `text` | setting.json | 695 / 101 | ImagePerception::observe（imageperception.cpp:50）；LLMService::retrieveMemory（llmservice.cpp:379） | 新请求或索引批次 | 视觉模型，空值使图片感知不可用 |

### 播放与服务

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `"api"` / `choice:api=GPT-SoVITS\|mock=模拟播放` | setting.json | 700 / 88 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | API 或模拟播放 |
| `"http://127.0.0.1:9880"` / `text` | setting.json | 701 / 通用或未单列 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | API 服务端点 |
| `""` / `file` | setting.json | 702 / 通用或未单列 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | 本地服务解释器 |
| `""` / `directory` | setting.json | 703 / 通用或未单列 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | 本地语音服务安装根目录 |

### 参考音频

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `""` / `reference_directory` | setting.json | 706 / 89 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | 空值用 app_data/reference_audio，从映射选择素材 |

### 模型文件

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `""` / `file` | setting.json | 709 / 通用或未单列 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | GPT 权重切换 |
| `""` / `file` | setting.json | 710 / 通用或未单列 | TTSService::reloadProvider（ttsservice.cpp:47）、ApiTTSProvider、TTSReferenceManager（ttsreferencemanager.cpp:46） | 播放安全边界或下一合成 | SoVITS 权重切换 |
| `""` / `directory` | setting.json | 711 / 通用或未单列 | ConfigManager（configmanager.cpp:620–637），无运行调用 | 隐藏、兼容读取 | 仅旧模型目录值 |
| `""` / `directory` | setting.json | 712 / 通用或未单列 | ConfigManager（configmanager.cpp:620–637），无运行调用 | 隐藏、兼容读取 | 仅旧模型目录值 |

### 对话记忆

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `1600` / `num:0:6000` | setting.json | 717 / 102 | CognitiveCoordinator::submit（cognitivecoordinator.cpp:293） | 下一条输入 | 0 关闭合并，批次最长等待仍由协调器控制 |
| `true` / `bool` | setting.json | 718 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 短期收录和窗口开关 |
| `15` / `num:0:999` | setting.json | 719 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 近期轮数 |
| `true` / `bool` | setting.json | 720 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 长期记忆与后台认知门控 |
| `15` / `num:1:999` | setting.json | 721 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:342）、MemoryPolicy（memorypolicy.cpp:175）、AppController（appcontroller.cpp:719、873） | 下一轮或权限收缩对账 | 摘要调度阈值 |

### 检索和索引

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `true` / `bool` | setting.json | 724 / 51 | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 索引维护和索引召回开关 |
| `false` / `bool` | setting.json | 725 / 52 | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 只回填空索引 |
| `40` / `num:10:200` | setting.json | 726 / 53 | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 候选和重排输入上限 |
| `true` / `bool` | setting.json | 727 / 54 | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 远程重排开关，关闭仍本地排序 |
| `10` / `num:1:40` | setting.json | 728 / 55 | MemoryIndexWorker（memoryindexworker.cpp:31、64）、MemoryRetrievalService（memoryretrieval.cpp:453、588）、LLMService（llmservice.cpp:391） | 下一查询或安全批次 | 最终结果上限，关闭重排仍裁剪 |

### 主动回忆工具

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `true` / `bool` | setting.json | 731 / 通用或未单列 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 工具和 Planner 入口开关 |
| `"tool"` / `choice:tool=模型工具\|legacy=独立规划` | setting.json | 732 / 64 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 工具或独立 Planner |
| `"native"` / `choice:native=原生调用\|json_envelope=JSON 信封` | setting.json | 733 / 65 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 原生工具或 JSON 信封 |
| `6000` / `num:1000:15000` | setting.json | 734 / 69 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 旧 Planner 周期及共享定向执行器预算，概览独立 |
| `2` / `num:1:4` | setting.json | 735 / 68 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 工具及规划轮数另受硬上限 |
| `8` / `num:1:20` | setting.json | 736 / 70 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 共享检索结果还会再夹紧 |

### 连接

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `false` / `bool` | setting.json | 741 / 通用或未单列 | AppController 差异应用（appcontroller.cpp:845）→ QQProtocolClient | 变化时重连 | QQ 渠道开关，不删除历史 |
| `"ws://127.0.0.1:3001"` / `text` | setting.json | 742 / 通用或未单列 | AppController 差异应用（appcontroller.cpp:845）→ QQProtocolClient | 变化时重连 | 连接地址 |
| `""` / `secret` | setting.json | 743 / 通用或未单列 | AppController 差异应用（appcontroller.cpp:845）→ QQProtocolClient | 变化时重连 | 传输鉴权 |

### 回复范围

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `"master"` / `choice:none=关闭\|master=仅主人\|allowlist=允许列表\|all=全部` | setting.json | 746 / 76 | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 私聊回复域 |
| `"none"` / `choice:none=关闭\|mention=仅被 @ 时\|all=全部` | setting.json | 747 / 75 | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 群回复模式与 @ 触发 |
| `false` / `bool` | setting.json | 748 / 74 | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 主人群例外 |
| `false` / `bool` | setting.json | 749 / 77 | QQReplyPolicy、MemoryPolicy（memorypolicy.cpp:156–206） | 下一入站 | 是否限制空回复名单 |
| `QJsonObject{}` / `group_permissions` | setting.json | 750 / 78 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 虚拟表格映射群记录和回复名单，检索另核权 |
| `QJsonObject{}` / `private_permissions` | setting.json | 751 / 79 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 虚拟表格映射私聊记录和回复名单，检索另核权 |

### 群聊记忆

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `false` / `bool` | setting.json | 754 / 通用或未单列 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 群记录总开关 |
| `30` / `num:1:3650` | setting.json | 755 / 84 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 隐藏，兼容读写，无到期清理 |
| `30` / `num:0:1440` | setting.json | 756 / 83 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 0 停止周期刷新 |
| `1440` / `num:1:10080` | setting.json | 757 / 82 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 成员证据缓存有效期 |
| `QJsonObject{}` / `modes` | setting.json | 758 / 80 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | 逐群覆盖记录模式 |
| `QJsonObject{}` / `senders` | setting.json | 759 / 81 | MemoryPolicy（memorypolicy.cpp:156–206）、GroupMembershipService（groupmembershipservice.cpp:42）、LLMService（llmservice.cpp:820） | 下一入站、查询或差异刷新 | strict 模式的发送者名单 |

### 群消息整理

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `15` / `num:1:1440` | setting.json | 762 / 通用或未单列 | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 空闲触发整理分钟数 |
| `50` / `num:1:500` | setting.json | 763 / 通用或未单列 | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 触发整理并限制批读取条数 |
| `90` / `num:10:3600` | setting.json | 764 / 85 | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 消息间隔超过后分段 |
| `20` / `num:1:100` | setting.json | 765 / 86 | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 段内事件数量 |
| `10` / `num:1:120` | setting.json | 766 / 87 | RawEventProcessor（raweventprocessor.cpp:105–123）、InteractionSegmenter（interactionsegmenter.cpp:68–75） | 下一整理批次 | 段内最长分钟数 |

### 近期群聊上下文

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `true` / `bool` | setting.json | 769 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 近期群窗口开关 |
| `30` / `num:1:1440` | setting.json | 770 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 回看时间窗 |
| `50` / `num:1:500` | setting.json | 771 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 每群候选 |
| `2` / `num:1:20` | setting.json | 772 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 主人相关群数量 |
| `10` / `num:1:100` | setting.json | 773 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 定向条数 |
| `1200` / `num:100:8000` | setting.json | 774 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 定向字数 |
| `50` / `num:1:500` | setting.json | 775 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 概览条数 |
| `2200` / `num:100:12000` | setting.json | 776 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 概览字数 |
| `10` / `num:1:100` | setting.json | 777 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 保留完整原文的近期条数 |
| `24` / `num:8:200` | setting.json | 778 / 通用或未单列 | LLMService::prepareReply（llmservice.cpp:865–895、1378–1390）、RecentContextPlanner（recentcontext.cpp:171、250） | 下一投影 | 较旧单条概览字数 |

### 记忆权限

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `false` / `bool` | setting.json | 783 / 71 | ActionPolicy::visibleSession、MemoryPolicy（memorypolicy.cpp:35）、AppController（appcontroller.cpp:228、257） | 下一轮，权限缩减对账 | 跨私聊个人记忆读取，对外披露仍复核 |
| `"r3"` / `text` | setting.json | 784 / 通用或未单列 | ActionPolicy::visibleSession、MemoryPolicy（memorypolicy.cpp:35）、AppController（appcontroller.cpp:228、257） | 下一轮，权限缩减对账 | 隐藏，仍写入原始事件作审计 |
| `QJsonArray{}` / `rules` | setting.json | 785 / 72 | ActionPolicy::visibleSession、MemoryPolicy（memorypolicy.cpp:35）、AppController（appcontroller.cpp:228、257） | 下一轮，权限缩减对账 | exact 为字面子串，pattern 为正则 |

### 身份与会话

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `QJsonArray{}` / `ids` | identity.json | 790 / 通用或未单列 | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 自身回声过滤 |
| `QJsonArray{}` / `ids` | identity.json | 791 / 通用或未单列 | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 主人身份与私域授权 |
| `QJsonArray{}` / `zones` | identity.json | 792 / 73 | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 会话隐私边界 |

### 桌宠表现

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `true` / `bool` | identity.json | 795 / 通用或未单列 | AppController 入站与设置（appcontroller.cpp:845）、MemoryPolicy（memorypolicy.cpp:156）、CognitiveCoordinator（cognitivecoordinator.cpp:285） | 下一事件或局部立即应用 | 控制外部渠道情绪是否影响桌宠 |

### 主动联系与回顾

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `false` / `bool` | identity.json | 800 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 主动外发授权 |
| `false` / `bool` | identity.json | 801 / 90 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 控制新源回顾和私下活动，Concern 和 OpenWake 独立 |
| `720` / `num:0:10080` | identity.json | 802 / 95 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 默认复查分钟数，0 等新依据 |
| `14` / `num:1:365` | identity.json | 803 / 96 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 定时复查和失败重试窗，不删历史 |
| `900` / `num:60:604800` | identity.json | 804 / 97 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 共享回顾间隔至少60秒 |
| `1800` / `num:0:604800` | identity.json | 805 / 92 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 空闲门槛，0 不额外等待 |
| `"23:00"` / `time` | identity.json | 806 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 安静起点 |
| `"08:00"` / `time` | identity.json | 807 / 通用或未单列 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 安静终点，起止相同无安静窗口 |
| `QJsonArray{}` / `targets` | identity.json | 808 / 91 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 主动联系目标仍核权 |

### 每日上限（三个上限相互独立）

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `3` / `num:0:999` | identity.json | 811 / 93 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 共享回顾预算，0 停止领取 |
| `12` / `num:0:96` | identity.json | 812 / 107 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 0 不领取，正值受共享预算 |
| `3` / `num:0:99` | identity.json | 813 / 94 | CognitiveCoordinator（cognitivecoordinator.cpp:228）、PrivateReview（privatereview.cpp:540）、MemoryManager（memorymanager.cpp:6257、6680）、ActionPolicy | 下一调度、提交、发送，撤权即时对账 | 主动投递上限，0 不限 |

### 内部唤醒与快照

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `false` / `bool` | identity.json | 816 / 105 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 启用内部调度 |
| `false` / `bool` | identity.json | 817 / 106 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 联系须额外满足主动权限和目标 |
| `30` / `num:1:100` | identity.json | 818 / 108 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 共享占比并受共享总限 |
| `4096` / `num:2048:8192` | identity.json | 819 / 109 | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 仅快照 JSON 大小 |
| `false` / `bool` | identity.json | 820 / 110 | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 实验，setter > 环境变量 > 配置 |

### 高级设置：调度与失败退避

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `900` / `num:60:86400` | identity.json | 823 / 111 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 仅初始化无有效计划时的延迟 |
| `1800` / `num:300:86400` | identity.json | 824 / 112 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 次次模型未给时间时默认值 |
| `300` / `num:300:86400` | identity.json | 825 / 113 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 后续安排最小间隔 |
| `86400` / `num:300:86400` | identity.json | 826 / 114 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 后续安排最大间隔 |
| `3` / `num:1:8` | identity.json | 827 / 115 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 诊断值无执行门控，隐藏 |
| `3` / `num:0:20` | identity.json | 828 / 116 | OpenWakeConfig::effectiveDailyCap、MemoryManager::claimOpenWake（memorymanager.cpp:6667）、PrivateReview（privatereview.cpp:474、1278） | 下一调度或提案提交 | 到期后允许先让出的普通任务数 |
| `QJsonArray{120, 300, 900}` / `ints` | identity.json | 829 / 117 | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 持久化每来源退避序列 |
| `4` / `num:1:10` | identity.json | 830 / 118 | CognitiveSnapshotBuilder（cognitivesnapshot.cpp:547）、LLMService（llmservice.cpp:4076）、MemoryManager::recordReviewFailure（memorymanager.cpp:6133） | 下一认知运行或失败 | 达到次数后暂停 |

### 检索服务（高级诊断）

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `false` / `bool` | setting.json | 835 / 56 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 关闭向量仍可词面匹配 |
| `""` / `choice:=未指定\|dashscope=阿里云百炼\|local_hash_test=本地测试` | setting.json | 836 / 57 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | DashScope 或已有诊断 hash |
| `""` / `text` | setting.json | 837 / 62 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 显式端点优先 |
| `"DASHSCOPE_API_KEY"` / `text` | setting.json | 838 / 61 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 配置密钥优先于向量环境变量 |
| `""` / `text` | setting.json | 839 / 58 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 模型身份须匹配向量索引 |
| `0` / `num:0:8192` | setting.json | 840 / 59 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 0 对应 DashScope 1024、本地 hash 64 |
| `""` / `text` | setting.json | 841 / 63 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 显式端点优先且须开启重排 |
| `"qwen3-rerank"` / `text` | setting.json | 842 / 60 | MemoryIndexWorker::run（memoryindexworker.cpp:96）、LLMService::retrieveMemory（llmservice.cpp:379）、MemoryRetrievalWorker::run（memoryretrievalworker.cpp:45） | 下一查询或安全批次 | 仅实际请求时应用 |

### 兼容性：Legacy Planner

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `true` / `bool` | setting.json | 845 / 67 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 仅 legacy，V1/V2 |
| `"memory_question"` / `choice:memory_question=回忆类问题\|always=每轮` | setting.json | 846 / 66 | LLMService（llmservice.cpp:714、1343、1697、4701、4896）、MemoryExecutor（memoryexecutor.cpp:104）、MemoryAgentPlanner（memoryagent.cpp:1094） | 下一轮 | 仅 legacy，决定独立规划是否触发 |

### 日志与诊断

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `""` / `log_directory` | setting.json | 849 / 99 | RuntimeLog::configure 与 AppController（appcontroller.cpp:845）、ConfigManager::loadSetting（configmanager.cpp:239） | 局部立即应用 | 空值使用 app_data/logs |
| `true` / `bool` | setting.json | 850 / 103 | RuntimeLog::configure 与 AppController（appcontroller.cpp:845）、ConfigManager::loadSetting（configmanager.cpp:239） | 局部立即应用 | 按启动独立或按日追加 |
| `false` / `bool` | setting.json | 851 / 104 | RuntimeLog::configure 与 AppController（appcontroller.cpp:845）、ConfigManager::loadSetting（configmanager.cpp:239） | 局部立即应用 | 诊断正文及请求日志 |

### 昼夜切换

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `"22:00"` / `time` | setting.json | 856 / 通用或未单列 | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 夜间开始 |
| `"07:00"` / `time` | setting.json | 857 / 通用或未单列 | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 白天开始 |

### 状态持续时间

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `30` / `num:1:3600` | setting.json | 860 / 通用或未单列 | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 脸红持续秒数 |
| `15` / `num:1:3600` | setting.json | 861 / 通用或未单列 | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 表情持续秒数 |
| `15` / `num:1:3600` | setting.json | 862 / 通用或未单列 | AppController::applyTimeSettings（appcontroller.cpp:810）→ TimeManager | 时间服务局部生效 | 距离状态持续秒数 |

### 角色提示词

| 设置 / 路径 | 文件 | 默认及类型范围 | 消费者 | 生效 | 条件与生命周期 |
|---|---|---|---|---|---|
| `""` / `multiline` | prompt.txt | 867 / 通用或未单列 | LLMService::loadSystemPrompt | 下次请求 | 读取 prompt.txt，保存校验非空 |

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
