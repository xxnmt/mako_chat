# 架构设计文档

> 项目：Windows_AI 桌面看板娘（桌宠）
> 版本：v0.6.0 + Phase 0/0.5 + Phase 1 前置（记忆库 v4）
> 更新日期：2026-10-27

---

## 1. 架构概览

### 1.1 架构风格

采用 **中央控制器 + 模块化设计**，以 `AppController` 为信号调度中枢，各功能模块独立实现，通过信号槽解耦。

### 1.2 模块划分

| 层级 | 模块 | 职责 | 文件 |
|------|------|------|------|
| **中枢层** | AppController | 应用生命周期管理、信号调度、模块组装 | appcontroller.h/cpp |
| **渠道层** | IChannelAdapter | 渠道抽象接口（channelName/sendUtterance） | ichanneladapter.h |
| | PetChannelAdapter | 桌宠渠道：气泡显示/播放跟读 | petchanneladapter.h/cpp |
| | QQProtocolClient | OneBot v11 WS 客户端：连接/重连/动作/事件 | qqprotocolclient.h/cpp |
| | QQMessageParser | OneBot 私聊/群聊事件解析（文本/@/媒体占位） | qqmessageparser.h/cpp |
| | QQReplyPolicy | 白名单/@ 门控：Drop / RecordOnly / Reply | qqreplypolicy.h/cpp |
| | QQChannelAdapter | QQ 文本输出（`send_private_msg`/`send_group_msg`） | qqchanneladapter.h/cpp |
| | SessionRouter | 输入路由：身份解析 + 会话历史组装 | sessionrouter.h/cpp |
| **UI层** | CharacterWidget | 立绘显示、鼠标拖拽、右键菜单 | characterwidget.h/cpp |
| | BubbleWidget | 气泡显示、打字机特效 | bubblewidget.h/cpp/ui |
| | ChatWidget | 聊天输入窗口、弹出跟随 | chatwidget.h/cpp |
| **业务层** | LLMService | DeepSeek请求、JSON协议解析、句子拆分、记忆提取 | llmservice.h/cpp |
| | PromptAssembler | 按渠道/可见性组装 system prompt 与短期记忆 messages（C3） | promptassembler.h/cpp |
| | GlobalMoodState | 全局情绪（emotion/blush/distance）；D1 控制外部情绪是否驱动桌宠立绘 | globalmoodstate.h/cpp |
| | TTSService | TTS 合成队列、策略模式（ITTSProvider）；播放交给 IAudioSink | ttsservice.h/cpp, ittsprovider.h, apittsprovider.h/cpp |
| | IAudioSink / PetAudioSink | 音频输出抽象 / 桌宠播放实现（播放器 + 播放队列） | iaudiosink.h, petaudiosink.h/cpp |
| | ApiTTSProvider | GPT-SoVITS API 调用、流式 PCM 解析 | apittsprovider.h/cpp |
| | TTSProcessManager | GPT-SoVITS 进程管理、孤儿进程清理（killProcessOnPort） | ttsprocessmanager.h/cpp |
| | TTSReferenceManager | 按情绪加载参考音频映射（tts_map.json） | ttsreferencemanager.h/cpp |
| | IPcmPlayer | 播放器抽象接口（静态工厂方法+PcmPlayerFinished/Error 信号） | ipcmplayer.h/cpp |
| | StreamPlayer | 流式 PCM 播放（QAudioSink + QBuffer，预填充+兜底检测） | streamplayer.h/cpp |
| | FilePlayer | 文件型 WAV 播放（QAudioSink + QFile，WAV头解析） | fileplayer.h/cpp |
| | AppearanceManager | 四维状态管理、立绘路径生成、换图触发 | appearancemanager.h/cpp |
| | TimeManager | 时间管理、表情/脸红退火（QTimer singleShot）、服装时段切换 | timemanager.h/cpp |
| | AnchorManager | 位置锚点管理、统一位置跟随 | anchormanager.h/cpp |
| | TagValidator | 标签合法性校验、编辑距离修正 | llmservice.h/cpp（内部类） |
| **数据层** | ConfigManager | API Key、记忆长度、时间/退火配置、单例模式 | configmanager.h/cpp |
| | MemoryManager | AI对话历史管理、SQLite数据库 | memorymanager.h/cpp |
| **设置层** | SettingsWidget | 设置界面（左右目录+滚动双向同步）、API/记忆/TTS模型配置 | settingswidget.h/cpp/ui |
| **数据结构** | SentenceText | 句子数据模型（中文/日文/标签） | sentencedata.h |
| | Utterance | 输出协议（text/ttsText/tags/channel/sessionId/speaker） | utterance.h |
| | RequestMeta | 输入元数据（channel/sessionId/speaker/subject/sensitive） | utterance.h |
| | HistoryTurn | 对话历史数据结构 | historyturn.h |
| | CharacterProfile | 角色档案数据结构（user/mako 主体） | historyturn.h |
| | EpisodicMemory | 情景记忆数据结构（事件/承诺/冲突/里程碑） | historyturn.h |
| | LongTermSummary | 长期摘要数据结构 | historyturn.h |
| | RelationshipState | 关系状态数据结构（亲密度/信任度） | historyturn.h |
| | AnchorStrategy | 锚点位置枚举与配置结构 | anchorstrategy.h |

### 1.3 架构总览图

```
┌─────────────────────────────────────────────────────────────────────┐
│                        AppController                                │
│                    （应用中枢 / 信号调度中心）                          │
│                                                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐     │
│  │ CharacterWidget │  │   BubbleWidget  │  │    ChatWidget   │     │
│  │ 立绘/拖拽/右键菜单 │  │   气泡/打字机    │  │   聊天输入框    │     │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘     │
│           │                    │                    │              │
│           │                    └────────┬───────────┘              │
│           │                             │                          │
│           ▼                             ▼                          │
│  ┌─────────────────┐           ┌─────────────────┐                │
│  │ AppearanceMgr   │           │  AnchorManager  │                │
│  │   四维状态/换图   │           │   位置锚点管理    │                │
│  └─────────────────┘           └─────────────────┘                │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                        LLMService                            │  │
│  │           AI对话/JSON解析 → ConfigManager(单例)               │  │
│  │           ← AI状态同步机制（方案C）                            │  │
│  │           ← MemoryManager(历史上下文+用户画像+摘要)            │  │
│  │           ← TagValidator(标签校验与修正)                      │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                        TTSService                            │  │
│  │  合成队列(Producer) → ITTSProvider(ApiTTSProvider)           │  │
│  │  ← 流式合成：pcmDataReady 信号 → StreamPlayer.writePcm()     │  │
│  │  ← 非流式合成：synthesisFinished → 临时WAV文件路径            │  │
│  │  播放队列(Consumer) → IPcmPlayer(StreamPlayer/FilePlayer)    │  │
│  │  ← playAudioAction 信号同步 UI（气泡+立绘）                  │  │
│  │  ← setSynthesisDone() 标记流式合成完成，触发播放结束检测      │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐    │
│  │ SettingsWidget  │  │ ConfigManager   │  │  MemoryManager  │    │
│  │   设置界面       │  │   单例配置管理   │  │   AI记忆系统     │    │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘    │
│                                                                     │
│  核心数据结构：SentenceText (zhText + jaText + rawTags)             │
│              HistoryTurn (userInput + rawReply)                     │
│  位置策略：AnchorStrategy (AnchorPosition枚举 + AnchorConfig)       │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.4 多渠道分层（Phase 0）

```
输入源（桌宠输入框 / QQ 私聊、群聊 / 定时或事件触发）
        │
        ▼
SessionRouter::submitInput(channel, sessionId, speaker, text, senderId)   // 桌宠 senderId 留空
        ├─ resolveSubject(channel, senderId) → subject（self→丢弃 / master→'user' / 未知→qq:<uin>）
        ├─ MemoryManager::getHistoryTurn(N, channel, sessionId)   ← 双键隔离
        └─ emit requestReply(text, history, meta)
                │
                ▼
        LLMService::askDeepSeek(text, history, meta)
                │  sentencesReady
                ▼
        AppController::handleMakoReply
                ├─ saveQATurn(..., meta) / 记忆提取（带 meta）
                └─ TTSService::enqueueSentences
                        │ playAudioAction
                        ▼
        AppController::onPlayAudioAction
                ├─ PetChannelAdapter::sendUtterance → BubbleWidget（渠道私有表现）
                ├─ GlobalMoodState::applyTags(..., pet)（全局情绪，D2）
                └─ AppearanceManager::applyTags（立绘表现；外部情绪经 D1 才驱动）
```

**边界**：QQ 文本链路已接线（WS 客户端→解析→策略→`raw_events`→`SessionRouter`）；QQ 语音/图片/表情包未实现；观察权/主动转述为 Phase 2 设计（见 HUMANIZATION_PLAN 6.4–6.6）。

---

## 2. 核心组件设计

### 2.1 AppController

**职责**：应用中央控制器，负责模块创建、信号连接、生命周期管理

**设计要点**：
- 唯一持有所有模块实例
- 统一管理信号槽连接
- 作为LLM回复→TTS→UI的协调中枢
- 集中错误处理
- 负责记忆提取触发与结果写入

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `startApp()` | 启动应用，显示角色 | - | - |
| `handleMakoReply()` | 接收LLM解析结果，保存记忆+入队TTS+触发记忆提取 | QList\<SentenceText\> sentences, QString rawReply | - |
| `handleSystemError()` | 统一错误处理，显示错误气泡+切悲伤立绘 | QString errorMsg | - |
| `onPlayAudioAction()` | TTS播放同步：更新气泡+立绘 | QString zhText, QMap tags | - |

**信号槽连接清单**（全部在构造函数中建立）：
```cpp
// 用户输入 → LLM请求（通过ChatWidget，含记忆查询）
connect(m_chatWidget, &ChatWidget::textSubmitted, this, [this](const QString &text){
    m_lastUserInput = text;
    int memoryLength = ConfigManager::instance().getShortMemoryLength();
    QList<HistoryTurn> shortTermMemory = m_memoryManager->getHistoryTurn(memoryLength);
    m_llmService->askDeepSeek(text, shortTermMemory);
});

// 右键菜单 → 设置界面
connect(m_character, &CharacterWidget::settingsRequested, m_settingsWidget, &SettingsWidget::show);

// 设置保存 → 更新LLMService API Key
connect(m_settingsWidget, &SettingsWidget::settingsSaved, this, [this](){
    m_llmService->setApiKey(ConfigManager::instance().getApiKey());
});

// TTS模型切换（GPT/SoVITS模型保存时）→ 通知TTS服务热切换
connect(m_settingsWidget, &SettingsWidget::ttsModelSwitchRequested,
        this, [this](const QString &gptPath, const QString &sovitsPath){
    m_ttsService->switchModel(gptPath, sovitsPath);
});

// LLM回复 → AppController → 保存记忆 + 入队TTS + 触发记忆提取
connect(m_llmService, &LLMService::sentencesReady, this, &AppController::handleMakoReply);

// TTS播放 → 同步UI（气泡+立绘）
connect(m_ttsService, &TTSService::playAudioAction, this, &AppController::onPlayAudioAction);

// 外观变化 → 角色立绘更新
connect(m_appearance, &AppearanceManager::characterPathChanged, m_character, &CharacterWidget::updatePath);

// 外观变化 → 位置更新
connect(m_appearance, &AppearanceManager::characterPathChanged, m_anchorManager, &AnchorManager::updateAllAnchors);

// 气泡显示 → 位置更新
connect(m_bubble, &BubbleWidget::bubbleShown, m_anchorManager, &AnchorManager::updateAllAnchors);

// 网络错误 → 统一处理
connect(m_llmService, &LLMService::internetErrorSignal, this, &AppController::handleSystemError);

// 记忆提取结果 → 写入角色档案/情景记忆/长期摘要/关系状态
connect(m_llmService, &LLMService::memoryExtractionReady, this, [this](const QJsonObject &extractionResult, qlonglong lastEndId, const QString &sourceIdsJson) {
    // 1. 角色档案更新（character_updates）
    QJsonArray charUpdates = extractionResult["character_updates"].toArray();
    for (const QJsonValue &val : charUpdates) {
        QJsonObject obj = val.toObject();
        m_memoryManager->upsertCharacterProfile(obj["subject"].toString(), obj["key"].toString(), obj["value"].toString());
    }
    // 2. 情景记忆写入（episodic_memories）
    QJsonArray episodicMemories = extractionResult["episodic_memories"].toArray();
    for (const QJsonValue &val : episodicMemories) {
        QJsonObject obj = val.toObject();
        m_memoryManager->addEpisodicMemory(obj["content"].toString(), obj["type"].toString("event"),
                                           obj["importance"].toDouble(0.5),
                                           QDateTime::fromString(obj["event_time"].toString(), "yyyy-MM-dd HH:mm:ss"),
                                           sourceIdsJson);
    }
    // 3. 工作摘要 → 写入长期摘要表（working_summary）
    QString summary = extractionResult["working_summary"].toString();
    if (!summary.isEmpty()) {
        m_memoryManager->addLongTermSummary(summary, lastEndId, sourceIdsJson);
    }
    // 4. 关系状态更新（relationship_updates）
    QJsonArray relUpdates = extractionResult["relationship_updates"].toArray();
    for (const QJsonValue &val : relUpdates) {
        QJsonObject obj = val.toObject();
        double delta = obj["delta"].toDouble(0.0);
        if (delta != 0.0) {
            m_memoryManager->upsertRelationshipState(obj["dimension"].toString(), delta);
        }
    }
});

// 状态提供者注册（AI状态同步）
m_llmService->registerStateProvider([this](){return m_appearance->getCurrentStateDescription();});
```

---

### 2.2 CharacterWidget

**职责**：角色立绘显示与鼠标交互

**设计要点**：
- 仅负责UI渲染和用户交互
- 不再直接持有LLMService和BubbleWidget（解耦）
- 通过 `updatePath()` 接收外观变化
- 立绘切换锚定**脚底中心**（两张图 bottom 均为 767，脚底对齐），角色原地生长不漂移
- 位置+尺寸用原子 `setGeometry` 一次到位，且 `setPixmap` 后先 `repaint()` 再移动，消除 resize/move 两步重绘造成的闪帧
- 构造时 `WA_NoSystemBackground`，减少透明窗口切换时的系统重绘

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `updatePath()` | 切换立绘图片（锚定脚底中心 + 原子 setGeometry） | QString imagePath | - |
| `calculateVisibleRect()` | 扫描像素透明度，计算有效区域 | QPixmap pixmap | QRect |
| `mousePressEvent()` | 记录拖拽起始偏移 | QMouseEvent* | - |
| `mouseMoveEvent()` | 移动窗口并发出角色移动 | QMouseEvent* | - |

**信号**：
| 信号 | 触发时机 | 参数 |
|------|----------|------|
| `chatRequested()` | 用户请求聊天（右键菜单） | - |
| `settingsRequested()` | 用户请求设置（右键菜单） | - |

---

### 2.3 BubbleWidget

**职责**：对话气泡组件，支持打字机特效

**设计要点**：
- 独立的无边框顶层窗口（Qt::Tool）
- 位置跟随由 AnchorManager 统一管理
- 通过 `bubbleShown()` 信号通知位置管理器更新位置

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `showMessage()` | 显示气泡并启动打字机，通知位置管理器 | QString text | - |
| `typeWriteEffect()` | 逐字显示文本（定时器触发） | - | - |
| `paintEvent()` | 绘制圆角气泡背景 | QPaintEvent* | - |

**信号**：
| 信号 | 触发时机 | 参数 |
|------|----------|------|
| `bubbleShown()` | 气泡显示时，通知位置管理器更新位置 | - |

---

### 2.4 LLMService

**职责**：AI对话服务，负责DeepSeek请求、JSON协议解析、标签校验

**设计要点**：
- 与UI完全解耦，纯业务逻辑
- 使用 JSON Mode（`response_format: {type: "json_object"}`）强制 LLM 输出合法 JSON
- 系统指令要求输出 `{"sentences": [...]}` 格式
- 将回复拆分为多句 `SentenceText`
- 每句独立解析 emotion/blush/distance/clothing 标签
- **状态提供者模式**：通过 `registerStateProvider()` 注册回调，实现AI状态同步
- **提示词外部化**：`initializePromptFile()` 和 `loadSystemPrompt()` 已实现
- **标签校验**：`TagValidator` 类实现标签合法性校验和编辑距离修正
- **上下文组装**：交给 `PromptAssembler`（档案/情景/摘要/关系按渠道可见性过滤；短期记忆的 proactive/群聊格式也在此处理）
- **提取带元数据**：`extractMemoryAsync` 接收 `RequestMeta`，信号原样回传；写入按本轮 meta 归属（渠道/会话/说话人/主体）

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `askDeepSeek()` | 发起DeepSeek API请求，构建系统提示词（基础prompt+角色档案+情景记忆+长期摘要+关系状态） | QString userInput, QList\<HistoryTurn\> historyQA | - |
| `onReplyFinished()` | 解析JSON→拆句→标签校验→发信号 | QNetworkReply* | - |
| `parseJsonReply()` | 解析JSON回复，提取句子和标签 | QString replyText | QPair\<QList\<SentenceText\>, QString\> |
| `extractMemoryAsync()` | 后台异步提取记忆（角色档案+情景记忆+长期摘要+关系状态），传入已有档案/记忆/关系做增量更新 | turns, lastEndId, sourceIds, existingProfiles, existingMemories, RequestMeta meta | - |
| `registerStateProvider()` | 注册状态提供者回调 | std::function\<QString()\> | - |
| `setMemoryManager()` | 设置MemoryManager实例 | MemoryManager* manager | - |
| `initializePromptFile()` | 初始化提示词文件 | - | - |
| `loadSystemPrompt()` | 从文件加载系统提示词 | - | QString |
| `setApiKey()` | 设置API Key | QString apiKey | - |

**信号**：
| 信号 | 触发时机 | 参数 |
|------|----------|------|
| `sentencesReady(sentences, rawReply)` | 回复解析完成 | QList\<SentenceText\>, QString |
| `internetErrorSignal(msg)` | 网络错误 | QString |
| `memoryExtractionReady(extractionResult, lastEndId, sourceIdsJson, meta)` | 记忆提取完成（结果含 character_updates/episodic_memories/working_summary/relationship_updates 四类字段；meta 为本轮渠道/会话/说话人/主体） | QJsonObject, qlonglong, QString, RequestMeta |

**JSON输出协议**：
```json
{
  "sentences": [
    {
      "zh_text": "中文文本",
      "ja_text": "日文翻译",
      "tags": {
        "emotion": "happyIdle",
        "blush": "unblushing",
        "distance": "far",
        "clothing": "schoolUniform"
      }
    }
  ]
}
```

**标签参数**：
| 标签 | 必填 | 可选值 | 说明 |
|------|------|--------|------|
| emotion | 是 | happyIdle, happyMore, amazing, loving, caring, sad, conscientious | 表情 |
| blush | 否 | unblushing, blushing | 脸红状态 |
| distance | 否 | far, closer | 距离 |
| clothing | 否 | pajama, schoolUniform, schoolUniformWithoutCap, schoolUniformWithoutCoat | 服装 |

**API请求参数**：
```cpp
rootObj["model"] = "deepseek-v4-flash";
rootObj["messages"] = messagesArray;
rootObj["temperature"] = 0.7;
rootObj["max_tokens"] = 8192;
rootObj["response_format"] = QJsonObject{{"type", "json_object"}};
```

---

### 2.5 TagValidator

**职责**：标签合法性校验与修正

**设计要点**：
- 支持精确匹配、大小写忽略匹配、编辑距离匹配、继承上一状态、使用默认值
- 编辑距离阈值根据标签长度动态调整

**校验流程**：
```
1. 精确匹配 → 直接通过
2. 大小写忽略匹配 → 修正大小写
3. 编辑距离匹配 → 修正拼写错误
4. 继承上一状态 → 使用上一句的标签值
5. 使用默认值 → 使用该标签的默认值
```

**合法标签值**：
| 标签 | 合法值列表 |
|------|------------|
| emotion | happyIdle, happyMore, amazing, loving, caring, sad, conscientious |
| blush | unblushing, blushing |
| distance | far, closer |
| clothing | pajama, schoolUniform, schoolUniformWithoutCap, schoolUniformWithoutCoat |

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `levenshteinDistance()` | 计算两个字符串的编辑距离 | QString s1, QString s2 | int |
| `validateTag()` | 校验并修正标签值 | tagName, rawValue, validValues, contextText, prevValue | QString |
| `getEditDistanceThreshold()` | 获取编辑距离阈值 | QString value | int |

**默认值**：
| 标签 | 默认值 |
|------|--------|
| emotion | happyIdle |
| blush | unblushing |
| distance | far |
| clothing | schoolUniform |

---

### 2.6 TTSService

**职责**：TTS 语音合成（句子 → 音频/PCM），把音频交给渠道音频输出；不直接管理播放器

**设计要点**：
- **策略模式**：通过 `ITTSProvider` 抽象接口支持多种 TTS 实现
- **合成队列**：`m_ttsQueue` + `m_isSynthesizing` 控制合成流水线
- **合成/播放分离**：合成结果交给 `IAudioSink`；桌宠由 `PetAudioSink` 播放，QQ 不设置 sink（只合成不播放）
- **播放器归桌宠**：播放器、播放队列、气泡/立绘时序都在 `PetAudioSink`，TTSService 只把 `sentenceStarted` 转成 `playAudioAction`
- **流式合成**：`ApiTTSProvider` 调用 GPT-SoVITS HTTP API，`streaming_mode=true`，通过 `pcmDataReady` 信号实时推送 PCM 数据
- **播放器抽象**：`IPcmPlayer` 接口统一 `StreamPlayer`（流式）和 `FilePlayer`（文件型）两种播放器
- **预填充防吞开头**：`startPlayer` 时先把队列中已到达的 PCM 写入 QBuffer，再 `QAudioSink::start`
- **播放完成检测**：`setSynthesisDone()` 标记合成结束，`onStateChanged(IdleState)` + `checkPlayEnd()` 兜底定时器双重检测
- **模拟回退**：合成失败时由 sink 走模拟播放（按字数估算时长）

**架构图**：
```
  SentenceText ──► TTSService（只合成）
                      │ enqueueSentences → m_ttsQueue → processTtsQueue
                      ▼
                  ApiTTSProvider.synthesize() (HTTP POST /tts)
                      │
                      ├─ 流式 pcmDataReady ──► IAudioSink::writePcm / updateSampleRate
                      └─ synthesisFinished ──► IAudioSink::endStream / playFile

  PetAudioSink（桌宠渠道，拥有播放器）
      ├─ beginStream：创建 StreamPlayer，首句立即起播
      ├─ 后续句：PCM 预缓冲，上一句播完后起播
      ├─ sentenceStarted ──► TTSService::playAudioAction ──► 气泡 + 立绘
      └─ 播放队列空 ──► idle ──► playbackQueueEmpty
```

**ITTSProvider 接口**：
```cpp
class ITTSProvider : public QObject {
    Q_OBJECT
public:
    virtual void synthesize(const SentenceText &sentence) = 0;
    virtual void warmUp() = 0;
    virtual bool isStreamingMode() const = 0;
signals:
    void synthesisFinished(const QString &audioPath, const SentenceText &sentence);
    void synthesisFailed(const QString &errorMsg, const SentenceText &sentence);
    void pcmDataReady(const QByteArray &pcmData);  // 流式专用
};
```

**IPcmPlayer 接口**（含静态工厂方法）：
```cpp
class IPcmPlayer : public QObject {
    Q_OBJECT
public:
    enum Type { Stream, File };
    static IPcmPlayer* create(Type type, QObject *parent = nullptr);  // 静态工厂

    virtual void startPlayer(int sampleRate, int channels, int sampleBits) = 0;
    virtual void stopPlayer() = 0;
    virtual void writePcm(const QByteArray &pcmData) = 0;
    virtual bool getisPlaying() const = 0;
    virtual void setSynthesisDone() {}  // 流式专用：标记合成完成
    virtual void setSource(const QString &path) {}  // 文件型专用：加载音频源
    virtual void updateSampleRate(int sampleRate) {}  // 采样率校正（流式播放器重写）
signals:
    void PcmPlayerFinished();
    void PcmPlayerError(const QString &error);
};
```

**依赖关系**：TTSService 只依赖 `IAudioSink` 抽象；`PetAudioSink` 负责创建 `IPcmPlayer`（工厂方法），并 include streamplayer/fileplayer 的工厂实现。

**音频输出接口（IAudioSink）**：
| 方法/信号 | 作用 |
|-----------|------|
| `beginStream(sentence, sampleRate)` | 开始一句流式合成（sink 决定是否立即起播） |
| `writePcm(pcm)` / `updateSampleRate(rate)` | 写入 PCM / 采样率校正 |
| `endStream(sentence)` / `abortPendingStream()` | 合成结束 / 异常中止 |
| `playFile(path, sentence, sampleRate)` | 文件型或模拟播放（空路径 = 模拟） |
| `isIdle()` / `stop()` | 是否空闲 / 停止并清理 |
| `sentenceStarted(sentence)` / `idle` | 开始播放某句（UI 同步） / 队列排空 |

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `enqueueSentences()` | 接收多句话，推入合成队列 | QList\<SentenceText\> | - |
| `reloadProvider()` | 根据配置重新加载 TTS Provider | - | - |
| `switchModel()` | 切换 GPT-SoVITS 模型 | gptPath, sovitsPath | - |
| `setAudioSink()` | 注入渠道音频输出（桌宠 PetAudioSink；空=只合成不播放） | IAudioSink* | - |
| `processTtsQueue()` | 取出一句调用 Provider 合成；流式把 PCM 转给 sink | - | - |
| `onTtsFinished()` | 合成完成：流式 `endStream` / 文件 `playFile`，继续合成 | audioPath, sentence | - |
| `onTtsFailed()` | 合成失败：中止流式缓冲并交 sink 模拟播放 | errorMsg, sentence | - |

**信号**：
| 信号 | 触发时机 | 参数 |
|------|----------|------|
| `playAudioAction(zhText, tags)` | sink 的 `sentenceStarted` 转发（桌宠播放开始时同步 UI） | QString, QMap\<QString,QString\> |
| `playbackQueueEmpty()` | 合成队列空且 sink 空闲（本轮全部播完） | - |

**StreamPlayer 播放完成检测机制**：
- **setSynthesisDone()**：PetAudioSink 在 `endStream` 中调用，标记所有 PCM 已到达
- **onStateChanged(IdleState)**：QAudioSink 状态变 Idle 时检查 `queueEmpty && bufferDone && synthesisDone`
- **checkPlayEnd() 兜底定时器**：500ms 间隔，检测 `queueEmpty && bufferDone && deviceEmpty`，连续3次（1.5秒）满足则判定播放完
- **finishAndEmit()**：统一停止定时器+emit PcmPlayerFinished

**预填充防吞开头机制**：
- StreamPlayer：startPlayer 时从 m_queue 取出所有已到达 PCM，写入 m_buffer，seek(0) 后再 `QAudioSink::start`
- FilePlayer：setSource 中 seek(44) 跳过 WAV 头后，预读取 CHUNK_SIZE*4 数据写入 m_buffer，再 resume()

**采样率自适应机制**：
- **流式模式**：ApiTTSProvider 在 readyRead 跳过头时从服务端 WAV 头偏移 24 读取真实采样率写入 `m_sampleRate`；TTSService 经私有 helper `apiSampleRate()`（内部 qobject_cast<ApiTTSProvider*>）取值传给 `startPlayer()`，首块到达时 StreamPlayer 用 `updateSampleRate()` 校正（与默认相同则空操作）
- **非流式模式**：FilePlayer 已具备 WAV 头解析逻辑，会从文件头读取采样率并自动重建 QAudioSink；非流式分段响应由 ApiTTSProvider 从首个 WAV 头读取采样率
- **采样率取值**：不再按 `super_sampling` 猜测，统一以服务端 WAV 头为准（v3=24000Hz，v1/v2=32000Hz，v4/超分=48000Hz），`m_sampleRate` 默认 24000 仅为兜底

---

### 2.7 AppearanceManager

**职责**：角色外观四维状态管理，负责立绘切换

**设计要点**：
- 维护四维状态：distance / clothing / blush / emotion
- blush 有退热机制（未显式指定时自动 unblushing）
- 路径变化时才发出 `characterPathChanged` 信号（防抖）
- 路径格式与资源目录结构严格对应
- **状态描述输出**：提供 `getCurrentStateDescription()` 用于AI状态同步
- **与 GlobalMoodState 的关系**：情绪三维同时写入全局状态；桌宠侧由 AppController::applyPetTags() 统一同步，服装仍只属于外观层

**状态机**：
```
距离: far ↔ closer
服装: pajama ↔ schoolUniform ↔ schoolUniformWithoutCap ↔ schoolUniformWithoutCoat
脸红: unblushing ↔ blushing（自动退热）
表情: happyIdle / happyMore / amazing / loving / caring / sad / conscientious
```

**路径生成规则**：
```
:/image/{distance}/{clothing}/{blush}/{emotion}.png
```

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `applyTags()` | 应用一组标签，触发换图检查 | QMap\<QString,QString\> | - |
| `getPath()` | 生成当前资源路径 | - | QString |
| `setDefault()` | 重置为默认状态 | - | - |
| `getCurrentStateDescription()` | 获取当前状态描述字符串 | - | QString |
| `getDistance()` | 获取当前距离 | - | QString |
| `getClothing()` | 获取当前服装 | - | QString |
| `getBlush()` | 获取当前脸红状态 | - | QString |
| `getEmotion()` | 获取当前表情 | - | QString |

**信号**：
| 信号 | 触发时机 | 参数 |
|------|----------|------|
| `characterPathChanged(newPath)` | 立绘路径变化时 | QString |

---

### 2.8 SentenceText（数据结构）

**职责**：贯穿LLM→TTS→UI的核心数据模型

```cpp
struct SentenceText {
    QString zhText;                  // 中文文本 → BubbleWidget显示
    QString jaText;                  // 日文文本 → TTS合成输入
    QMap<QString, QString> rawTags;  // 四维状态标签 → AppearanceManager换图
    bool isValidated;                // 标签是否已校验
};
```

**流转路径**：
```
LLMService::parseJsonReply() → TagValidator校验
    → AppController::handleMakoReply()
        → MemoryManager::saveQATurn()（持久化）
        → TTSService::enqueueSentences()（入队）
            → playAudioAction信号 → UI更新
```

---

### 2.9 ConfigManager

**职责**：全局配置管理（单例），支持配置文件持久化

**设计要点**：
- Meyers单例，线程安全
- 配置文件路径：`app_data/config/setting.json`
- 首次启动自动创建配置文件，使用默认值
- 支持 API Key、GPT-SoVITS 服务地址、短期记忆长度配置

**配置项**：
| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| api.deepseek_api_key | `sk-placeholder-key` | DeepSeek API密钥 |
| api.gpt_sovits_url | `http://127.0.0.1:9880` | GPT-SoVITS服务地址 |
| memory.short_term_length | `15` | 短期记忆轮数 |
| memory.summary_length | `15` | 长期记忆自动摘轮数（TD-037） |
| time.night_start | `22:00` | 夜晚开始时间（TD-037） |
| time.night_end | `07:00` | 夜晚结束时间（TD-037） |
| time.blush_lasting | `30` | 脸红退火时长（秒，TD-037） |
| time.emotion_lasting | `15` | 表情退火时长（秒，TD-037） |
| time.distance_lasting | `15` | 距离退火时长（秒，TD-037） |

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `loadSetting()` | 从JSON文件加载配置 | - | bool |
| `saveSetting()` | 保存配置到JSON文件 | - | bool |
| `getApiKey()` | 获取API Key | - | QString |
| `setApiKey()` | 设置API Key | QString | - |
| `getTTSUrl()` | 获取TTS服务地址 | - | QString |
| `getShortMemoryLength()` | 获取短期记忆长度 | - | int |
| `getSummaryMemoryLength()` / `setSummaryMemoryLength()` | 长期记忆自动摘轮数 | - / int | int / - |
| `getNightStartTime()` / `setNightStartTime()` | 夜晚开始时间 | - / QTime | QTime / - |
| `getNightEndTime()` / `setNightEndTime()` | 夜晚结束时间 | - / QTime | QTime / - |
| `getBlushLasting()` / `setBlushLasting()` | 脸红退火时长（秒） | - / int | int / - |
| `getEmotionLasting()` / `setEmotionLasting()` | 表情退火时长（秒） | - / int | int / - |
| `getDistanceLasting()` / `setDistanceLasting()` | 距离退火时长（秒） | - / int | int / - |
| `getConfigDirPath()` | 获取配置目录路径 | - | QString |
| `getMemoryPath()` | 获取数据库文件路径 | - | QString |

---

### 2.10 AnchorManager

**职责**：位置锚点管理器，统一协调所有跟随角色的Widget位置

**设计要点**：
- 通过 `eventFilter` 监听 CharacterWidget 的 Move/Resize 事件
- 支持预定义锚点策略和自定义位置计算函数
- 立绘切换时通过信号连接自动更新位置

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `registerWidget()` | 注册Widget到预定义锚点位置 | QWidget*, AnchorConfig | - |
| `unregisterWidget()` | 注销Widget | QWidget* | - |
| `updateAllAnchors()` | 计算并更新所有锚点位置 | - | - |
| `calculatePosition()` | 根据锚点策略计算目标位置 | QRect, QPoint, QSize, AnchorConfig | QPoint |

**位置更新触发机制**：
| 触发条件 | 机制 |
|----------|------|
| 角色移动 | eventFilter 捕获 QEvent::Move |
| 角色大小变化 | eventFilter 捕获 QEvent::Resize |
| 立绘切换 | characterPathChanged 信号 |
| 气泡显示 | bubbleShown 信号 |

---

### 2.11 ChatWidget

**职责**：聊天输入窗口，支持弹出和输入

**设计要点**：
- 独立的顶层窗口（Qt::Tool）
- 位置跟随由 AnchorManager 统一管理
- 回车键提交，自动隐藏

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `popup()` | 弹出聊天窗口并激活 | - | - |
| `showEvent()` | 显示时自动聚焦输入框 | QShowEvent* | - |

**信号**：
| 信号 | 触发时机 | 参数 |
|------|----------|------|
| `textSubmitted(text)` | 用户提交文本时 | QString |

---

### 2.12 SettingsWidget

**职责**：设置界面，管理API Key、记忆长度、长期记忆摘轮数、时间/退火配置、TTS模型切换和对话历史管理

**设计要点**（v0.6.0 重构为「主-从式滚动布局」，替代原多标签页）：
- 独立窗口（Qt::Window），支持关闭按钮
- **布局**：左侧目录树 `treeIndex` + 右侧滚动区 `scrollArea`/`contentWidget`（`widgetResizable`），内容按区块竖排
- **两级分区**：一级区域 `section1_*`（对话系统/语音合成/记忆系统/时间管理/历史会话记录）内含二级分区 `section2_*`（大语言模型/GPT-Sovits/短期记忆/长期记忆/昼夜切换/自动退火）
- **双向同步 scroll-spy**：点击目录项 → `ensureWidgetVisible` 滚到对应分区；滚动右侧 → 按分区页面位置（`mapTo(viewport).y()` 取最贴近视口顶部的分区）高亮左侧对应目录项。用 `m_sectionMap`（目录项↔分区映射）+ `m_syncGuard` 双向互斥防抖
- **滚轮透传**：下拉框/微调框/时间编辑框安装 `eventFilter`，滚轮事件转给右侧滚动条并消费，避免拦截界面滚动
- 显示时自动加载当前配置，配置修改后立即保存并通知AppController更新

**当前实现**：
- API Key配置（对话系统→大语言模型区）
- 短期记忆轮数（记忆系统→短期记忆区）
- TTS模型切换（语音合成→GPT-Sovits区：GPT路径/模型、SoVITS路径/模型，保存时 `emit ttsModelSwitchRequested`）
- 对话历史管理（历史会话记录区：表格展示、分页浏览、单条删除、清空全部，删除需API Key验证）
- 长期记忆自动摘轮数（记忆系统→长期记忆区，保存到 `memory.summary_length`，TD-037）
- 时间管理（时间管理区：夜晚开始/结束时间、脸红/表情/距离退火时长，保存到 `time.*`，TD-037）

**待完善**：
- 外观配置（气泡样式/窗口透明度）、快捷键配置

**信号**：
| 信号 | 触发时机 | 接收方 |
|------|----------|--------|
| `settingsSaved()` | 任一配置保存成功 | AppController → 更新LLMService API Key + `applyTimeSettings()` + 重启 TimeManager |
| `ttsModelSwitchRequested(gptPath, sovitsPath)` | GPT/SoVITS模型保存成功 | AppController → `m_ttsService->switchModel()` |

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `show()` | 显示设置窗口 | - | - |
| `setMemoryManager()` | 注入MemoryManager实例 | MemoryManager* | - |
| `setMemoryLength()` | 初始化记忆长度设置 | int length | - |
| `buildSync()` | 建立目录↔分区映射并连接双向同步 | - | - |
| `refreshHistoryTurnList()` | 刷新历史记录列表 | - | - |
| `loadHistoryPage()` | 加载指定页的历史记录 | int page | - |
| `eventFilter()` | 滚轮透传 | QObject*, QEvent* | bool |
| `on_btn_saveApiKey_clicked()` | 保存API Key | - | - |
| `on_btn_deleteSelectedMemory_clicked()` | 删除选中记录 | - | - |
| `on_btn_claenAllMemory_clicked()` | 清空全部记录 | - | - |

---

### 2.13 MemoryManager（已实现 · AI记忆系统）

**职责**：管理AI对话历史记录、角色档案、情景记忆、长期摘要和关系状态

**设计要点**：
- 使用SQLite存储三层数据：原始事件流水（`raw_events`）+ 对话轮次（`chat_history`）+ 蒸馏记忆（档案/情景/摘要/关系）；数据库版本 `PRAGMA user_version = 4`
- 每次对话后保存用户输入、AI回复（JSON对象）
- 对话前读取最近N轮历史作为短期记忆
- 角色档案区分 `user` / `mako` 两个主体，长期稳定特质
- 情景记忆按重要度阈值激活检索，支持衰减与状态流转
- 关系状态量化 user 与 AI 的关系维度（intimacy/trust），由 LLM 输出 delta 驱动
- 支持长期记忆摘要的自动生成和管理
- 数据库路径：`app_data/memory/QianDaoMoZi_memory.db`
- 注：旧的 `user_profile` 表已被 `character_profile` 表取代（通过 subject 字段区分 user/mako）

**数据库结构**（`PRAGMA user_version = 4`；v1/v2 基础 + v3 记忆库调整 + v4 会话隔离，合计 7 张表）：

**表1：chat_history（对话历史）**
| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY AUTOINCREMENT | 记录ID |
| timestamp | DATETIME DEFAULT (datetime('now','localtime')) | 创建时间（本地时间） |
| user_input | TEXT（可空） | 用户输入；主动/感知轮次为 NULL（v3） |
| raw_reply | TEXT NOT NULL | AI回复的JSON对象 `{"sentences": [...]}` |
| channel | TEXT NOT NULL DEFAULT 'pet' | 渠道：pet / qq_private / qq_group（v2） |
| session_id | TEXT NOT NULL DEFAULT 'default' | 会话 ID（QQ 为好友号/群号，v2） |
| speaker | TEXT | 说话人（群聊场景，v2） |
| sensitive | INTEGER NOT NULL DEFAULT 0 | 记忆盲区/敏感标记（v2） |
| turn_kind | TEXT NOT NULL DEFAULT 'reply' | 轮次类型：reply / proactive / perception（v3） |
| trigger_event_ids | TEXT | 触发事件 id 列表（JSON，指向 raw_events.id，v3） |
| subject | TEXT NOT NULL DEFAULT 'user' | 本轮对象：user / qq:<uin> / group:<群号>（v3） |

**表2：long_term_summary（长期记忆摘要）**
| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY AUTOINCREMENT | 记录ID |
| summary_text | TEXT NOT NULL | 摘要内容 |
| covered_turn_end_id | INTEGER NOT NULL | 覆盖的对话结束ID |
| source_ids | TEXT NOT NULL | 来源对话ID列表（JSON） |
| is_dirty | INTEGER DEFAULT 0 | 是否待重建 |
| channel | TEXT NOT NULL DEFAULT 'pet' | 渠道分桶（v2） |
| session_id | TEXT NOT NULL DEFAULT 'default' | 会话分桶（v2） |
| created_at | DATETIME DEFAULT (datetime('now','localtime')) | 创建时间 |
| updated_at | DATETIME DEFAULT (datetime('now','localtime')) | 更新时间 |

索引：`idx_summary_covered_end`(covered_turn_end_id)、`idx_summary_is_dirty`(is_dirty)

**表3：character_profile（角色档案，取代已废弃的 user_profile）**
| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY AUTOINCREMENT | 记录ID |
| subject | TEXT NOT NULL | 主体：`user` 或 `mako` |
| key | TEXT NOT NULL | 档案维度名（如 nickname/occupation/persona/性格） |
| value | TEXT NOT NULL | 档案值 |
| updated_at | DATETIME DEFAULT (datetime('now','localtime')) | 更新时间 |

约束：`UNIQUE(subject, key)`（同主体同维度唯一）

**表4：episodic_memory（情景记忆）**
| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY AUTOINCREMENT | 记录ID |
| content | TEXT NOT NULL | 事件描述 |
| event_time | DATETIME DEFAULT (datetime('now','localtime')) | 事件时间 |
| importance | REAL DEFAULT 0.5 | 重要度 0.0-1.0 |
| type | TEXT DEFAULT 'event' | 类型：event/promise/conflict/milestone |
| status | TEXT DEFAULT 'active' | 状态：active/resolved/broken |
| last_accessed | DATETIME DEFAULT (datetime('now','localtime')) | 最后访问时间（检索时更新） |
| last_decay_at | DATETIME DEFAULT (datetime('now','localtime')) | 最后衰减计算时间 |
| source_ids | TEXT | 来源对话ID列表（JSON） |
| origin | TEXT NOT NULL DEFAULT 'pet' | 渠道溯源：pet / qq_private / qq_group / bilibili / screen（v2） |
| visibility | TEXT NOT NULL DEFAULT 'private' | 可见性：private / channel / public（v3） |
| subject | TEXT NOT NULL DEFAULT 'user' | 记忆所属主体（v3） |
| session_id | TEXT NOT NULL DEFAULT 'default' | 会话（QQ 群号/好友号；桌宠 default，v4） |

索引：`idx_episodic_importance`(importance)、`idx_episodic_type`(type)、`idx_episodic_status`(status)

**表5：relationship_state（关系状态）**
| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY AUTOINCREMENT | 记录ID |
| subject | TEXT NOT NULL DEFAULT 'user' | 主体：`user`（主人）/ `qq:<uin>` / `group:<群号>`（v2 重建表） |
| dimension | TEXT NOT NULL | 维度名：`intimacy`(亲密度) / `trust`(信任度) |
| value | REAL NOT NULL | 维度值 0-100 |
| updated_at | DATETIME DEFAULT (datetime('now','localtime')) | 更新时间 |

约束：`UNIQUE(subject, dimension)`（v2 重建；存量行复制为 `subject='user'`）

初始化数据：`intimacy=30.0`、`trust=30.0`（initDatabase 时若表为空自动插入）

**表6：raw_events（原始事件流水，v3 由 raw_messages 泛化；本阶段只建不写）**
| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY AUTOINCREMENT | 记录ID |
| platform | TEXT NOT NULL | qq / bilibili / screen / system |
| channel | TEXT NOT NULL | pet / qq_private / qq_group / bilibili / screen |
| session_id | TEXT NOT NULL | 会话（群号/好友号/默认） |
| message_id | TEXT | 平台消息 ID（`UNIQUE(channel, message_id)` 去重） |
| direction | TEXT NOT NULL DEFAULT 'in' | in / out / event |
| sender_id | TEXT | 稳定身份（uin/uid；自己为 'self'） |
| sender_name | TEXT | 显示名快照 |
| event_type | TEXT NOT NULL | message / at / notice / percept / trigger / outbound |
| content | TEXT | 纯文本（可空） |
| payload | TEXT | JSON 原文（媒体/元数据） |
| occurred_at | DATETIME DEFAULT (datetime('now','localtime')) | 事件时间 |
| sensitive | INTEGER NOT NULL DEFAULT 0 | 盲区/敏感标记 |
| visibility | TEXT NOT NULL DEFAULT 'private' | private / channel / public |
| processed | INTEGER NOT NULL DEFAULT 0 | 是否已被蒸馏 |

索引：`idx_raw_events_session(channel, session_id, occurred_at)`、`idx_raw_events_processed(processed)`

**表7：memory_cursor（蒸馏游标，v3 新增）**
| 字段 | 类型 | 说明 |
|------|------|------|
| channel | TEXT NOT NULL | 渠道 |
| session_id | TEXT NOT NULL | 会话 |
| last_event_id | INTEGER NOT NULL DEFAULT 0 | 已蒸馏到的 raw_events.id |
| last_turn_id | INTEGER NOT NULL DEFAULT 0 | 已蒸馏到的 chat_history.id |
| updated_at | DATETIME DEFAULT (datetime('now','localtime')) | 更新时间 |

约束：`PRIMARY KEY(channel, session_id)`

**关键方法**：

**对话历史 CRUD**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `saveQATurn()` | 保存单轮对话（raw_reply 为 sentences JSON，meta 写入渠道/会话/说话人/敏感标记） | userInput, rawReply, sentences, meta=RequestMeta() | bool |
| `getHistoryTurn(N)` | 按渠道+会话双键获取最近N轮对话 | int N, channel="pet", sessionId="default" | QList\<HistoryTurn\> |
| `getHistoryTurn(offset, limit)` | 分页获取对话 | int offset, int limit | QList\<HistoryTurn\> |
| `getTotalHistoryCount()` | 获取记录总数 | - | qlonglong |
| `deleteTurnByID(id)` | 删除单条记录 | int id | bool |
| `clearAllHistory()` | 清空所有记录 | - | bool |
| `getUnsummarizedTurns()` | 按双键获取 id > long_term_summary.max(covered_turn_end_id) 的未摘要对话 | outLastEndId, outSourceIds, channel, sessionId | QList\<HistoryTurn\> |

**原始事件与蒸馏游标（v3）**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `appendRawEvent()` | 写入 raw_events（`INSERT OR IGNORE`，按 message_id 去重；本阶段不接线） | platform, channel, sessionId, eventType, content, messageId, direction, senderId, senderName, payloadJson, occurredAt, sensitive, visibility | bool |
| `updateMemoryCursor()` | 更新每 channel+session 的蒸馏游标 | channel, sessionId, lastEventId, lastTurnId | bool |
| `getMemoryCursorEventId()` | 读取事件游标（无记录返回 0） | channel, sessionId | qlonglong |

**角色档案 CRUD**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `upsertCharacterProfile()` | 插入或更新档案（先 normalizeProfileKey 归一化 key，已存在则 mergeProfileValue 合并 value） | subject, key, value | bool |
| `getCharacterProfiles(subject)` | 获取档案（subject 为空时返回全部） | QString subject | QList\<CharacterProfile\> |
| `deleteCharacterProfile()` | 删除档案 | id | bool |

**情景记忆 CRUD**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `addEpisodicMemory()` | 新增情景记忆（eventTime 无效时用当前时间） | content, type, importance, eventTime, sourceIds, origin="pet", visibility="private", subject="user", sessionId="default" | bool |
| `getActiveEpisodicMemories()` | 获取活跃记忆（status='active' AND importance≥minImportance，按 importance DESC 排序，并更新 last_accessed；可按 origin/visibility/sessionId 过滤） | minImportance=0.2, limit=20, origin="", visibility="", sessionId="" | QList\<EpisodicMemory\> |
| `deleteEpisodicMemory()` | 删除记忆 | id | bool |
| `updateEpisodicMemoryStatus()` | 更新记忆状态（如 promise 兑现 → resolved / 失约 → broken） | id, status | bool |
| `decayEpisodicMemory()` | 衰减扫描：importance≥0.8 不衰减；其余 -0.05/天；<0.1 删除 | - | int（删除条数） |

**长期记忆摘要 CRUD**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `addLongTermSummary()` | 添加摘要（按渠道双键分桶） | summaryText, coveredEndId, sourceIdsJson, channel, sessionId | bool |
| `getLatestSummaries()` | 获取最新摘要（is_dirty=0，按双键过滤） | limit=5, channel, sessionId | QList\<LongTermSummary\> |
| `deleteLongTermSummary()` | 删除摘要 | id | bool |

**关系状态 CRUD**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `upsertRelationshipState()` | 按 subject：维度不存在时初始化为 clamp(30+delta,0,100)；存在时 clamp(oldVal+delta,0,100) | dimension, delta, subject="user" | bool |
| `getRelationshipStates()` | 获取指定主体的全部关系维度（按 dimension 排序） | subject="user" | QList\<RelationshipState\> |
| `initRelationshipState()` | 若表为空，插入 intimacy=30、trust=30 | - | bool |

**工具函数**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `normalizeProfileKey()` | 同 subject 内基于编辑距离匹配已有 key，避免语义重复（全匹配→模糊匹配→新 key） | subject, rawKey | QString |
| `mergeProfileValue()` | 合并 value：newVal 非空且更长则采纳 newVal，否则保留 oldVal | key, oldVal, newVal | QString |
| `levenshteinDistance()` | 计算两个字符串的 Levenshtein 编辑距离 | s1, s2 | int |
| `getEditDistanceThreshold()` | 根据 value 长度动态返回编辑距离阈值（≤4字=1，≤8字=2，≤12字=3，>12字=4） | value | int |

**数据结构**：

```cpp
// 对话历史
struct HistoryTurn {
    qlonglong id = -1;
    QDateTime timestamp;
    QString userInput;
    QString rawReply;  // JSON对象: {"sentences": [...]}
};

// 角色档案（长期稳定特质，user 或 mako）
struct CharacterProfile {
    qlonglong id = -1;
    QString subject;    // 'user' 或 'mako'
    QString key;        // 如 'nickname'/'occupation'/'persona'
    QString value;
    QDateTime updatedAt;
};

// 情景记忆（事件、承诺、冲突、里程碑）
struct EpisodicMemory {
    qlonglong id = -1;
    QString content;    // 事件描述
    QDateTime eventTime;
    double importance = 0.5;   // 0.0-1.0
    QString type;       // 'event'/'promise'/'conflict'/'milestone'
    QString status;     // 'active'/'resolved'/'broken'
    QDateTime lastAccessed;
    QString sourceIds;
};

// 长期记忆摘要
struct LongTermSummary {
    qlonglong id = -1;
    QString summaryText;
    qlonglong coveredTurnEndId = -1;
    QString sourceIds;
    bool isDirty = false;
    QDateTime createdAt;
    QDateTime updatedAt;
};

// 关系状态（量化 user 与 AI 之间的关系维度）
struct RelationshipState {
    QString dimension;   // 'intimacy'(亲密度) / 'trust'(信任度)
    double value = 0.0;  // 0-100
    QDateTime updatedAt;
};
```

**记忆流程**：
```
用户提交输入 → getHistoryTurn(N) 获取短期记忆
    → askDeepSeek(userInput, historyQA, meta) 发送请求
        （PromptAssembler：按渠道可见性注入 角色档案 → 情景记忆 → 长期摘要 → 关系状态 → 当前状态）
    → AI回复 → saveQATurn(userInput, rawReply, sentences, meta) 保存记忆
    → 判断未摘要对话数 >= 阈值
        → getCharacterProfiles(meta.subject)+mako + getActiveEpisodicMemories(origin, session) 取已有上下文
        → extractMemoryAsync(turns, lastEndId, sourceIds, existingProfiles, existingMemories, meta)
    → 提取结果 memoryExtractionReady(result, lastEndId, sourceIdsJson, meta)：
        ├── character_updates   → upsertCharacterProfile()
        ├── episodic_memories   → addEpisodicMemory()
        ├── working_summary     → addLongTermSummary()
        └── relationship_updates → upsertRelationshipState() (delta≠0 才更新)
应用启动时 → decayEpisodicMemory() 执行一次情景记忆衰减
```

**角色档案 key 归一化**：
- `upsertCharacterProfile()` 写入前调用 `normalizeProfileKey(subject, key)`，同 subject 内基于 Levenshtein 编辑距离匹配已有 key
- 编辑距离阈值按 key 长度动态调整：≤4字=1，≤8字=2，≤12字=3，>12字=4
- 解决同义 key 碎片化问题（如"性格"与"个性"归并到同一维度）

**角色档案 value 合并策略**：
- `mergeProfileValue(key, oldVal, newVal)`：newVal 非空且长度 ≥ oldVal 时采纳 newVal，否则保留 oldVal
- 倾向保留更完整的描述，避免新提取的短值覆盖已有长值

**情景记忆衰减机制**（`decayEpisodicMemory()`）：
- 衰减规则：
  - importance ≥ 0.8 的里程碑/承诺不衰减（长期保留）
  - 其余按 -0.05/天 衰减
  - importance < 0.1 且 status='active' 的记忆自动删除
- 衰减间隔：距 `last_decay_at` ≥ 0.0416 天（约1小时）才执行，避免重复扣分
- 事务执行：UPDATE 衰减 + DELETE 清理在同一 transaction 内完成
- 调用时机：AppController 构造时调用一次

**情景记忆激活检索**：
- `getActiveEpisodicMemories(minImportance=0.2, limit=20)`：仅返回 status='active' 且 importance ≥ 阈值的记忆
- 排序：importance DESC, event_time DESC
- 检索副作用：批量更新命记忆的 `last_accessed` 字段

**关系状态机制**：
- 当前维度：`intimacy`（亲密度）、`trust`（信任度），值域 0-100
- 初始化：initDatabase 时若 relationship_state 表为空，自动插入 intimacy=30、trust=30
- 更新驱动：extractMemoryAsync 的 LLM 输出 `relationship_updates` 字段（dimension + delta），delta 范围 -5.0 ~ +5.0
- `upsertRelationshipState(dimension, delta)`：维度不存在时初始化为 clamp(30+delta, 0, 100)；存在时 clamp(oldVal+delta, 0, 100)
- AppController 仅在 delta ≠ 0 时调用 upsert，避免无变化时浪费写入

**LLM 注入格式**（askDeepSeek 中按顺序追加到 system prompt，均为 Markdown 段落）：
- `# 茉子的人设`：mako 主体档案，每行 `- key: value`
- `# 关于欧尼酱`：user 主体档案，每行 `- key: value`
- `# 我们的回忆`：情景记忆，每行 `- [yyyy-MM-dd] 内容（类型标注）`（承诺显示状态、冲突/里程碑显示标签）
- `# 最近发生`：长期摘要，每行 `- 摘要文本`
- `# 我们的关系`：关系状态，每行 `- 维度名: 值/100`（intimacy→亲密度，trust→信任度）

**AI 摘要增量更新**（extractMemoryAsync）：
- AppController 调用前先 `getCharacterProfiles("user")` + `getActiveEpisodicMemories(0.2, 20)` + `getRelationshipStates()` 取已有上下文
- Prompt 注入已有角色档案、已有情景记忆、当前关系状态三段上下文
- LLM 输出四类字段：`character_updates` / `episodic_memories` / `working_summary` / `relationship_updates`
- 提取规则：key 优先复用已有档案 key；矛盾信息以新对话为准；只输出有更新/新增的项；不简单复制已有档案

---

### 2.14 TimeManager（时间管理器，独立子类）

**职责**：时间驱动状态管理，负责表情/脸红退火倒计时与服装时段切换

**设计要点**：
- 独立子类，从 AppController 职责中拆出（缓解 TD-013 上帝对象问题）
- 使用 `QTimer::singleShot` 替代原 `QElapsedTimer` 状态机，避免野指针崩溃（TD-030）
- 退火时间单位修正：原 `hasExpired(15)` 误为 15ms，现 `start(15000)` 正确为 15秒（TD-032）
- 三个独立退火定时器：表情退火 + 脸红退火 + 距离退火，互不干扰
- 退火由「每轮对话最终状态」驱动，而非逐句触发（重构：删除 notifyLLMEnded/notifyLLMtagsApplicated/notifyBlushingApplicated 三个方法，统一为 notifyRoundEnded）
- 昼夜判定（onMinuteTick）跨午夜正确处理：`m_nightStart > m_nightEnd` 时用 `isNight = (now >= m_nightStart || now < m_nightEnd)`，修复整个白天被误判为夜晚（TD-035）
- 退火时长与夜晚时段由 ConfigManager 配置驱动（TD-037）：AppController 启动时 `applyTimeSettings()`；设置保存后重新应用并 `start()` 立即重估昼夜
- 退火门控（TD-034）：退火只对最新一轮生效——AppController 维护 `m_roundSeq`（每次用户输入 +1）与 `m_lastPlaybackRound`（本轮 TTS 入队时记录），`playbackQueueEmpty` 仅在两者相等时调用 `notifyRoundEnded()`；旧轮收尾被跳过，等新一轮播完再按最终状态处理

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `notifyRoundEnded(finalTags)` | 每轮结束，先停全部退火，再按最终状态启动对应定时器 | 最终状态 QMap | - |
| `notifyUserInputStarted()` | stop 三个退火定时器（用户输入打断退火） | - | - |
| `setDistanceTime(time)` | 设置距离退火时长（秒，与脸红/表情 setter 同格式） | int | - |
| `onMinuteTick()` | 每分钟检查服装时段切换（白天校服 / 夜晚睡衣） | - | - |

**退火机制**（按每轮最终状态）：
```
每轮对话结束 → playbackQueueEmpty → AppController 取最终视觉状态(finalTags)
    → TimeManager::notifyRoundEnded(finalTags) → 停全部定时器
    │
    ├── finalTags.blush == "blushing"  → 启动 m_blushResetTimer(m_blushTime*1000)
    ├── finalTags.emotion != "happyIdle" → 启动 m_emotionResetTimer(m_BackIdleTime*1000)
    ├── finalTags.distance == "closer" → 启动 m_distanceResetTimer(m_distanceTime*1000)
    │
    ├── 用户输入 → notifyUserInputStarted() → stop 全部定时器
    └── 各定时器触发 → 对应维度回归默认（表情→happyIdle / 脸红→unblushing / 距离→far）
```

---

### 2.15 TTSProcessManager（GPT-SoVITS 进程管理）

**职责**：管理 GPT-SoVITS python 进程的生命周期，清理孤儿进程

**设计要点**：
- `apiStart()` 开头通过 TCP 探测端口占用，若被占用则调用 `killProcessOnPort()` 清理孤儿 python.exe（TD-031）
- 修复 "SoVITS 切换 400 Bad Request"（端口被占用导致复用坏实例）
- 修复 "崩溃后留孤儿 python.exe"（异常退出后端口被占用）
- Windows 平台使用 `netstat` + `taskkill` 实现进程清理

**关键方法**：
| 方法 | 作用 | 参数 | 返回值 |
|------|------|------|--------|
| `apiStart()` | 启动 API（开头探测端口占用 → killProcessOnPort 清理孤儿） | - | - |
| `killProcessOnPort(int port)` | 清理占用指定端口的进程（Windows: netstat+taskkill） | int port | - |

---

### 2.16 多渠道适配层（Phase 0）

**职责**：把「大脑」与「渠道表现」解耦。大脑只产出 `Utterance`，渠道只负责自己的表现；渠道无权判定身份归属。

| 组件 | 职责 | 关键点 |
|------|------|--------|
| `Utterance` | 输出单元：text/ttsText/tags/channel/sessionId/speaker | 纯数据结构，无 Qt 依赖 |
| `RequestMeta` | 输入元数据：channel/sessionId/speaker/subject/sensitive | 默认 `pet/default/user`，旧调用零改动 |
| `IChannelAdapter` | `channelName()` + `sendUtterance()` 纯虚接口 | 大脑侧不感知具体渠道 |
| `PetChannelAdapter` | 桌宠渠道：气泡显示 | 持 `BubbleWidget*` 非所有权；立绘/情绪不在此层 |
| `QQChannelAdapter` | QQ 文本输出 | 经 `QQProtocolClient` 发 `send_private_msg`/`send_group_msg`；语音/图片待扩展 |
| `QQProtocolClient` | OneBot WS 客户端 | 连接 SnowLuma WS 服务端；重连/鉴权/动作/事件 |
| `QQMessageParser` | 入站解析 | 私聊/群聊、speaker、`mentionedSelf`、媒体占位、原始 JSON |
| `QQReplyPolicy` | 回复策略 | 白名单、`master_any_group`、群聊 mention-only；未通过 Drop |
| `SessionRouter` | 输入路由：身份解析 → 双键历史组装 → `requestReply` | `submitInput(..., senderId)`；self→丢弃、master→`user`、未知 QQ→`qq:<uin>`；启动时加载 identity.json |

**调用时序**：
```
ChatWidget::textSubmitted(text)
  → SessionRouter::submitInput("pet", "default", "欧尼酱", text)   // senderId 留空 → 主人
      ├─ resolveSubject → 'user'（QQ：self 丢弃 / master user / 未知 qq:<uin>）
      ├─ 组装 RequestMeta（subject/channel/session/speaker）
      ├─ getHistoryTurn(memoryLength, "pet", "default")
      └─ emit requestReply(text, history, meta)
  → AppController lambda：m_lastMeta=meta → notifyUserInputStarted() → askDeepSeek(text, history, meta)
  → handleMakoReply：saveQATurn(..., meta) + 记忆提取（按 meta 双键）
  → onPlayAudioAction：PetChannelAdapter 显示气泡；AppearanceManager 应用四维标签
```

**设计约束**：
- 渠道私有表现（气泡/IM 发送/语音消息）只进适配器；情绪是全局层（GlobalMoodState），立绘表现归桌宠（D2）
- 外部渠道情绪是否驱动桌宠立绘由 D1 `followExternalMood` 控制（`identity.json [appearance]`）；退火仍只由桌宠渠道触发
- 身份归属由宿主配置层决定（`SessionRouter::resolveSubject`），模型与渠道无权判定（C2）；`identity.json` 启动即加载
- 记忆按 `channel + session_id` 双键隔离，摘要与未摘要游标同样按双键分桶（A1）

---

### 2.17 PromptAssembler（上下文组装）

**职责**：把「取数据」与「拼上下文」分开；只做格式化与可见性过滤，不持有数据库。

| 方法 | 作用 |
|------|------|
| `buildSystemPrompt(base, meta, MemoryContext)` | 基础 prompt + 茉子人设 + 主人档案 + 情景记忆 + 摘要 + 关系状态；群聊过滤关系数值与亲密档案 key |
| `buildHistoryMessages(history, meta)` | 短期记忆 messages（含 history note）；proactive/perception 只发 assistant；群聊用户消息带说话人前缀 |
| `isEpisodicVisible()` | 桌宠全可见（主人特权）；`public` 全渠道；`channel` 要求 `origin + sessionId` 匹配；`private` 仅桌宠 |
| `isUserProfileVisible()` | 群聊过滤含「关系/称呼/情感/依赖/性格」的档案 key |

**记忆上下文结构**：`MemoryContext { userProfiles, makoProfiles, memories, summaries, relationships }`，由 `LLMService` 取数后传入。

---

### 2.18 GlobalMoodState（全局情绪）

**职责**：保存跨渠道的 `emotion / blush / distance`（D2：情绪全局、立绘表现桌宠私有）。

| 方法/信号 | 作用 |
|-----------|------|
| `applyTags(tags, sourceChannel)` | 更新全局情绪；服装不参与 |
| `setFollowExternalMood(bool)` | D1：外部渠道情绪是否驱动桌宠立绘（来自 `identity.json [appearance]`） |
| `getEmotion()/getBlush()/getDistance()` | 读取全局情绪 |
| `getCurrentStateDescription(clothing)` | 与 AppearanceManager 同格式的状态描述（服装由外观层提供） |
| `externalMoodApplied(moodTags)` | 仅当来源非桌宠且 D1 开启时发出；AppController 收到后应用到桌宠立绘（只三维，不含服装） |

**接线**：
- 桌宠：`AppController::applyPetTags()` → `AppearanceManager::applyTags()` + `GlobalMoodState::applyTags(..., pet)`
- 外部渠道（QQ）：`GlobalMoodState::applyTags(..., qq)` →（D1 开启）`AppearanceManager::applyTags(三维)`；不触发 TimeManager

---

### 2.19 QQ 渠道（文本已接入）

**职责**：OneBot v11 文本收发；QQ 语音/图片/表情包与主动观察为后续批次。

**入站链路**：
```
SnowLuma WS 服务端 (:3001)
  → QQProtocolClient::messageEvent(QJsonObject)
  → QQMessageParser::parse（channel/session/speaker/senderId/mentionedSelf/rawJson）
  → QQReplyPolicyEngine::decide（Drop / RecordOnly / Reply）
  → raw_events（仅非 Drop）
  → SessionRouter::submitInput（仅 Reply）
```

**出站链路**：
```
AppController::handleMakoReply
  ├─ pet：TTSService（气泡/TTS）
  └─ qq_private / qq_group：QQChannelAdapter::sendUtterance
        → QQProtocolClient::sendAction(send_private_msg / send_group_msg)
        （不触发桌宠气泡/TTS；情绪写入 GlobalMoodState，D1 决定是否驱动立绘）
```

**策略语义**：
- `private`：`none` / `master` / `allowlist` / `all`；`allowlist` 自动包含 master
- `group`：`none` / `mention` / `all`；`group_allow` 非空时只处理这些群
- 未通过白名单：不写 `raw_events`、不回复
- 通过群但未 @：只写 `raw_events`，不进大脑
- `retcode=100` 视为瞬时发送失败，4 秒后重试一次

**配置**：`setting.json` 的 `qq` 段（`enabled/ws_url/access_token/reply`）。

---

## 3. 核心数据流

### 3.1 完整对话流程

```
1. 用户输入
   │
   ▼
2. CharacterWidget::chatRequested() → ChatWidget::popup() → 用户输入 → textSubmitted(input)
   │
   ▼
3. SessionRouter::submitInput("pet", "default", "欧尼酱", input)   ← Phase 0
   │
   ├── resolveSubject(channel, senderId) → 'user'（self 丢弃 / master user / 未知 qq:<uin>）
   ├── 读取短期记忆: getHistoryTurn(memoryLength, channel, sessionId)
   └── emit requestReply(text, history, meta)
   │
   ▼
3.5 AppController lambda
   │
   ├── 保存 m_lastUserInput / m_lastMeta
   ├── TimeManager::notifyUserInputStarted()
   └── 调用 askDeepSeek(userInput, shortTermMemory, meta)
   │
   ▼
4. LLMService::askDeepSeek()
   │
   ├── 构建系统提示词（基础prompt + 角色档案 + 情景记忆 + 长期摘要 + 关系状态）
   ├── 构建短期记忆（纯文本注入，不含标签）
   ├── 注入当前环境状态（UI状态 + 当前时间，作为独立 system message）
   ├── 构建JSON请求体（含 response_format: json_object）
   └── 发送POST到DeepSeek API
   │
   ▼
5. API响应 → onReplyFinished()
   │
   ├── 提取content文本
   ├── 解析JSON对象: {"sentences": [...]}
   ├── TagValidator校验标签合法性
   ├── 生成修正后的JSON
   └── 发射 sentencesReady(sentences, correctedReply)
   │
   ▼
6. AppController::handleMakoReply()
   │
   ├── saveQATurn(userInput, correctedReply, sentences, m_lastMeta) 保存对话（含渠道/会话/主体）
   ├── pet：enqueueSentences(sentences) 入队TTS
   ├── QQ：QQChannelAdapter::sendUtterance 发文本（不触发桌宠 TTS/气泡）
   └── 检查是否触发记忆提取
   │
   ▼
7. TTSService::enqueueSentences()
   │
   ├── 入合成队列
   └── 启动合成流水线
   │
   ▼
8. 合成队列（Producer）
   │
   ├── 取出一句 → ApiTTSProvider.synthesize(HTTP POST)
   └── 继续取下一句
   │
   ▼
9. 音频输出（PetAudioSink，桌宠渠道）
   │
   ├── 流式：beginStream/writePcm → 首块PCM到达 → sentenceStarted → playAudioAction
   ├── 后续句：PCM预缓冲 → 上一句播完 → 起播并切换气泡
   └── 非流式：playFile → FilePlayer读取文件播放
   │
   ▼
10. AppController::onPlayAudioAction(zhText, tags)
    │
    ├── BubbleWidget::showMessage(zhText) → 打字机显示
    ├── GlobalMoodState::applyTags(tags, pet) → 全局情绪同步
└── AppearanceManager::applyTags(tags) → 立绘切换
```

### 3.2 历史记忆注入

```
读取历史记录 → 遍历每个HistoryTurn
    │
    ├── 用户输入: 直接注入
    │
    └── AI回复: 
        ├── 尝试解析JSON对象
        │   └── 成功 → 提取zh_text拼接为纯文本
        └── 解析失败
            └── 剥离[xxx:yyy]格式标签 → 纯文本
        
        注入为: {"role": "assistant", "content": "纯文本内容"}
```

**设计决策**：只注入纯文本，不注入标签格式，避免影响LLM的输出格式。

**v3 轮次渲染规则**：`reply` 轮注入 `user` + `assistant`；`proactive` / `perception`（或 `user_input` 为 NULL）只注入 `assistant`，不生成空 `user` 消息；记忆提取 prompt 对主动/感知轮输出「茉子（主动）: …」，不生成「用户:」空行。

---

## 4. 资源管理

### 4.1 立绘资源结构

```
image/
├── closer/                    # 近景（大尺寸）
│   ├── pajama/
│   │   ├── blushing/ (7张)
│   │   └── unblushing/ (7张)
│   ├── schoolUniform/
│   ├── schoolUniformWithoutCap/
│   └── schoolUniformWithoutCoat/
└── far/                       # 远景（小尺寸）
    └── (同上结构)
```

### 4.2 资源数量

- 距离：2种（far / closer）
- 服装：4种（pajama / schoolUniform / schoolUniformWithoutCap / schoolUniformWithoutCoat）
- 脸红：2种（unblushing / blushing）
- 表情：7种（happyIdle ~ conscientious）
- **总计**：2 × 4 × 2 × 7 = **112张立绘**

---

## 5. 设计模式

| 模式 | 应用位置 | 作用 |
|------|----------|------|
| **单例模式** | ConfigManager | 全局配置唯一实例 |
| **观察者模式** | 信号槽系统（AppController为中枢） | 模块间松耦合通信 |
| **生产者-消费者** | TTSService（合成）→ PetAudioSink（播放队列） | 合成与播放解耦，渠道可替换输出 |
| **策略模式** | TTSService（ITTSProvider接口） | 多种TTS实现可切换 |
| **适配器模式** | IChannelAdapter（PetChannelAdapter/QQChannelAdapter） | 渠道表现可插拔，大脑不感知渠道 |
| **上下文对象** | RequestMeta | 渠道/会话/主体元数据随轮次贯穿全链路 |
| **多态接口** | IPcmPlayer（StreamPlayer/FilePlayer） | 统一播放器生命周期管理 |
| **静态工厂方法** | IPcmPlayer::create(Type, parent) | 集中创建逻辑，TTSService 不依赖具体子类 |
| **状态管理** | AppearanceManager | 角色外观状态机 |
| **编辑距离算法** | TagValidator | 标签拼写修正 |

---

## 6. 代码规范

### 6.1 命名规范

| 类型 | 规则 | 示例 |
|------|------|------|
| 类名 | PascalCase | CharacterWidget, AppController |
| 成员变量 | m_前缀 + camelCase | m_llmService, m_visibleRect |
| 函数名 | camelCase | askDeepSeek, calculateVisibleRect |
| 信号 | camelCase | sentencesReady, characterPathChanged |
| 结构体 | PascalCase | SentenceText |

### 6.2 信号命名约定

- 已发生事件：`xxxReady` / `xxxChanged` / `xxxFinished`
- 错误事件：`xxxErrorSignal`
- 用户动作：`userXxx`

---

## 7. 技术债务与优化项

### 7.1 已修复问题

| 编号 | 问题 | 修复方案 | 状态 |
|------|------|----------|------|
| IC-001 | 服务定位器反模式 | 构造函数注入apiKey | ✅ 已修复 |
| RC-001~RC-011 | 代码冗余与死代码清理 | 逐个清理 | ✅ 已修复 |
| TD-005 | TTS为模拟实现 | 接入GPT-SoVITS API | ✅ 已修复 |
| TD-010 | AI记忆系统未实现 | 实现MemoryManager | ✅ 已修复 |
| TD-009 | 系统提示词硬编码 | 外部化prompt.txt | ✅ 已修复 |
| TD-018 | 流式PCM解析丢失数据 | readyRead跳过首个WAV头后透传裸PCM | ✅ 已修复 |
| TD-019 | StreamPlayer野指针崩溃 | m_buffer初始化nullptr + signals去重 | ✅ 已修复 |
| TD-020 | 播放完成信号不触发 | setSynthesisDone+IdleState+兜底定时器 | ✅ 已修复 |
| TD-021 | 播放吞开头 | startPlayer预填充PCM再启动QAudioSink | ✅ 已修复 |
| TD-022 | FilePlayer写入位置错误 | pushData先seek到末尾再write | ✅ 已修复 |
| TD-023 | TTSService直接new具体播放器 | IPcmPlayer静态工厂方法 | ✅ 已修复 |
| TD-024 | 衰减重复扣分 | user_profile 表整体删除，confidence/last_triggered 字段不再存在 | ✅ 已废弃 |
| TD-025 | tier 保留逻辑反转 | tier 字段随 user_profile 表删除 | ✅ 已废弃 |
| TD-026 | TTS 文本切分错误 | cut0（不切）→ cut5（按全部标点切） | ✅ 已修复 |
| TD-027 | 超分采样率不匹配 | m_sampleRate 动态适配（流式 qobject_cast / 非流式 WAV 头解析） | ✅ 已修复 |
| TD-028 | 角色档案 key 碎片化 | normalizeProfileKey 带 subject 参数，服务于 character_profile | ✅ 已修复 |
| TD-029 | AI 摘要盲提取 | extractMemoryAsync 传 existingProfiles + existingMemories 增量更新 | ✅ 已修复 |
| TD-030 | TimeManager 野指针崩溃 | QElapsedTimer 指针未初始化 → 重构为 QTimer singleShot | ✅ 已修复 |
| TD-031 | TTSProcessManager 孤儿进程 | apiStart 开头 killProcessOnPort 清理占端口孤儿 python.exe | ✅ 已修复 |
| TD-032 | TimeManager 退火时间单位错误 | hasExpired(15) 误为 15ms → QTimer::start(15000) 正确为 15秒 | ✅ 已修复 |
| TD-035 | 昼夜判定写反（跨午夜分支比较符用错，整个白天被误判为夜晚） | onMinuteTick 跨午夜分支改为 `(now >= m_nightStart || now < m_nightEnd)` | ✅ 已修复 |
| TD-036 | 立绘 far/closer 切换漂移与闪帧 | updatePath 锚定脚底中心 + 原子 setGeometry + setPixmap 后 repaint + WA_NoSystemBackground | ✅ 已修复 |
| BUG-001 | 未摘要历史查询恒为空（bind 后 exec(sql) 丢参数） | 改为 `query.exec()` 复用预编译语句 | ✅ 已修复（61f1767） |
| BUG-002 | 空标签被编辑距离误判为枚举首项 | 修正 Levenshtein 空串边界 | ✅ 已修复（485b5e3） |

### 7.2 待优化项

| 编号 | 问题 | 位置 | 说明 | 状态 |
|------|------|------|------|------|
| A-001 | AppController职责过重 | appcontroller.cpp | 持有全部模块实例（TimeManager 已独立为子类） | ⚠️ 可接受 |
| C-003 | 内联lambda过多 | appcontroller.cpp | 难以测试和复用 | ⚠️ 待优化 |
| C-004 | LLMService头文件依赖MemoryManager | llmservice.h | 已用 `class MemoryManager;` 前向声明（TD-015 已修复） | ✅ 已修复 |
| RC-012 | AnchorManager析构未清理 | anchormanager.cpp | 非问题：仅持弱引用，widget 所有权归 AppController（TD-014 已结案） | ✅ 非问题 |
| TD-034 | 对话轮冲突导致退火误触发 | appcontroller.cpp + timemanager.cpp | 上一轮播放收尾的 playbackQueueEmpty 晚于新一轮输入，被误认为当前轮结束，用旧轮 finalTags 启动退火定时器，中途提前退火 | ✅ 已修复（轮次门控：`playbackQueueEmpty` 仅在 `m_lastPlaybackRound == m_roundSeq` 时退火） |
| TD-037 | 设置界面三组控件未接线 | settingswidget.cpp / configmanager.* / timemanager.cpp | 长期记忆摘轮数、夜晚时段、退火时长控件与保存按钮已就位，但无槽函数、ConfigManager 无配置项、TimeManager 硬编码默认值 | ✅ 已修复（`memory.summary_length`/`time.*` 配置 + 三个保存槽 + `applyTimeSettings()` 热应用） |

### 7.3 架构演进规划

| 阶段 | 版本 | 内容 |
|------|------|------|
| 当前 | v0.6.0 | 流式TTS播放、IPcmPlayer抽象（静态工厂）、播放完成检测、预填充防吞开头、TimeManager QTimer singleShot 退火、TTSProcessManager 孤儿进程清理 |
| 已完成 | Phase 0 | 大脑/渠道解耦：Utterance/IChannelAdapter/SessionRouter、DB v2 双键记忆、identity.json 解析、23 例测试 |
| 下一步 | Phase 0.5 → Phase 1 | 记忆 visibility/appendRawMessage 预留 → SnowLuma/OneBot v11 QQ 渠道接入 |
| 中期 | v0.7.0 | 设置界面完善、AppController职责拆分、独立存档系统 |
| 远期 | v0.8.0+ | 架构改进、独立存档、插件化 |

---

## 8. 扩展点

| 扩展方向 | 接入方式 |
|----------|----------|
| 新LLM模型 | 继承/替换 LLMService，保持 sentencesReady 信号接口 |
| 新TTS引擎 | 实现 ITTSProvider 接口，流式需发 pcmDataReady 信号 |
| 新播放器类型 | 实现 IPcmPlayer 接口，在 ipcmplayer.cpp 工厂方法中注册新类型 |
| 新服装/表情 | 添加资源文件，AppearanceManager自动支持 |
| 设置界面 | 连接 ConfigManager 的setter方法 |
| 新交互方式 | 在CharacterWidget中添加新信号，连接到AppController |
| 新消息渠道 | 实现 `IChannelAdapter`，输入经 `SessionRouter::submitInput(channel, sessionId, speaker, text, senderId)` 进入大脑 |
| 新记忆渠道隔离 | `RequestMeta.channel/sessionId` 随轮次传递，MemoryManager 按双键读写 |
| 插件系统 | AppController中注册新模块，通过信号槽通信 |
