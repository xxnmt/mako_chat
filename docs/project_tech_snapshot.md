# 【项目技术快照】

> 项目：Windows_AI 桌面看板娘（桌宠）
> 日期：2026-10-27
> 版本：v0.7.0-rc5 + Phase 1 前置（记忆库 v4）+ QQ 文本
> 状态：开发中（Phase 0/0.5 与 Phase 1 前置项已完成；Phase 1 主体 QQ 文本已通）

---

## 1. 核心类与成员

### AppController（应用中枢）

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_character` | CharacterWidget*，角色组件 |
| | `m_bubble` | BubbleWidget*，气泡组件 |
| | `m_llmService` | LLMService*，AI服务 |
| | `m_appearance` | AppearanceManager*，外观管理 |
| | `m_globalMood` | GlobalMoodState*，全局情绪（D2；外部情绪驱动受 D1 控制） |
| | `m_ttsService` | TTSService*，TTS服务 |
| | `m_qqClient` | QQProtocolClient*，OneBot WS 客户端（入站落 raw_events） |
| | `m_chatWidget` | ChatWidget*，聊天输入窗口 |
| | `m_anchorManager` | AnchorManager*，位置锚点管理 |
| | `m_settingsWidget` | SettingsWidget*，设置界面 |
| | `m_memoryManager` | MemoryManager*，AI记忆系统 |
| | `m_timeManager` | TimeManager*，时间管理器（表情/脸红退火、服装时段切换） |
| | `m_lastUserInput` | QString，最后一次用户输入 |
| | `m_roundSeq` | quint64，对话轮次序号（每次用户输入 +1，TD-034） |
| | `m_lastPlaybackRound` | quint64，最近一次 TTS 入队所属轮次（退火门控，TD-034） |
| **函数签名** | `void startApp()` | 启动应用，显示角色 |
| | `void handleMakoReply(const QList<SentenceText>& sentences, const QString& rawReply)` | 处理AI回复，保存记忆+入队TTS+触发记忆提取 |
| | `void handleSystemError(const QString& errorMsg)` | 统一错误处理 |
| | `void onPlayAudioAction(const QString& zhText, const QMap<QString,QString>& tags)` | 播放同步：更新气泡+立绘 |
| | `void applyPetTags(const QMap<QString,QString>& tags)` | 桌宠侧：立绘（含服装）+ 全局情绪同步 |
| | `void initConnections()` | 集中初始化信号槽连接 |

### CharacterWidget

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `dragPosition` | QPoint，鼠标拖拽偏移 |
| | `imageLabel` | QLabel*，立绘图片标签 |
| | `m_visibleRect` | QRect，立绘有效像素区域 |
| **函数签名** | `void updatePath(const QString& imagePath)` | 切换立绘图片（锚定脚底中心 + 原子 setGeometry，避免 closer 下坠漂移与切换闪帧） |
| | `QPoint getBubbleAnchorPos() const` | 获取气泡锚点坐标（已不再直接使用） |
| | `QPoint getChatAnchorPos() const` | 获取聊天窗口锚点坐标 |
| | `QRect getVisibleRect() const` | 获取立绘有效区域 |
| | `QRect calculateVisibleRect(const QPixmap& pixmap)` | 计算立绘有效区域 |
| **信号** | `void chatRequested()` | 用户请求聊天（右键菜单触发） |
| | `void settingsRequested()` | 用户请求设置（右键菜单触发） |

### AnchorManager（位置锚点管理）

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_character` | CharacterWidget*，角色组件 |
| | `m_anchors` | std::unordered_map\<QWidget*, AnchorInfo\>，锚点映射 |
| **函数签名** | `void registerWidget(QWidget* widget, AnchorConfig config)` | 注册Widget到预定义锚点位置 |
| | `void registerWidget(QWidget* widget, std::function<QPoint()> customCalculator)` | 注册Widget到自定义位置计算 |
| | `void unregisterWidget(QWidget* widget)` | 注销Widget |
| | `void updateAllAnchors()` | 更新所有锚点位置 |
| | `QPoint calculatePosition(const QRect& visibleRect, const QPoint& characterPos, const QSize& widgetSize, const AnchorConfig& config)` | 根据配置计算目标位置 |

### AnchorStrategy（锚点策略）

| 枚举 | 值 | 说明 |
|------|------|------|
| AnchorPosition | Bottom | 底部 |
| | WaistCenter | 腰部居中 |
| | WaistLeft | 腰部左侧 |
| | WaistRight | 腰部右侧 |
| | TopCenter | 顶部居中 |
| | HeadRight | 头部右侧 |
| | Custom | 自定义 |

### ChatWidget（聊天输入窗口）

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_lineEdit` | QLineEdit*，输入框 |
| **函数签名** | `void popup()` | 弹出聊天窗口 |
| **信号** | `void textSubmitted(const QString& text)` | 用户提交文本 |

### AppearanceManager

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_distance` | QString，距离：far/closer |
| | `m_clothing` | QString，服装：pajama/schoolUniform/schoolUniformWithoutCap/schoolUniformWithoutCoat |
| | `m_blush` | QString，脸红：unblushing/blushing |
| | `m_emotion` | QString，表情：happyIdle/happyMore/amazing/loving/caring/sad/conscientious |
| | `m_lastPath` | QString，上次路径缓存 |
| **函数签名** | `void applyTags(const QMap<QString,QString>& tags)` | 应用标签，触发换图 |
| | `QString getPath() const` | 生成资源路径 |
| | `void setDefault()` | 重置为默认状态 |
| | `QString getCurrentStateDescription() const` | 获取当前状态描述字符串 |
| | `QString getDistance() const` | 获取当前距离 |
| | `QString getClothing() const` | 获取当前服装 |
| | `QString getBlush() const` | 获取当前脸红状态 |
| | `QString getEmotion() const` | 获取当前表情 |
| **信号** | `void characterPathChanged(const QString& newPath)` | 路径变化信号 |

### LLMService

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_networkManager` | QNetworkAccessManager*，网络请求管理器 |
| | `m_apiKey` | QString，API Key（构造函数注入） |
| | `m_localPromptPath` | QString，提示词文件路径 |
| | `m_systemPromptCache` | QString，系统提示词缓存 |
| | `m_stateProvider` | std::function\<QString()\>，状态提供者回调 |
| | `m_memoryManager` | MemoryManager*，AI记忆系统 |
| **函数签名** | `LLMService(const QString& apiKey, QObject* parent)` | 构造函数，接收API Key参数 |
| | `void askDeepSeek(const QString& userInput, const QList<HistoryTurn>& historyQA, const RequestMeta& meta = RequestMeta())` | 发起DeepSeek请求（JSON协议），记忆上下文交 PromptAssembler 按渠道组装 |
| | `void onReplyFinished(QNetworkReply* reply)` | 解析JSON回复→拆句→标签校验→发信号 |
| | `QPair<QList<SentenceText>, QString> parseJsonReply(const QString& replyText)` | 解析JSON回复，提取句子和标签 |
| | `void extractMemoryAsync(turns, lastEndId, sourceIds, existingProfiles, existingMemories, const RequestMeta& meta = RequestMeta())` | 异步提取记忆；prompt 带渠道/会话/说话人/主体，结果按本轮 meta 归属 |
| | `void registerStateProvider(std::function<QString()> provider)` | 注册状态提供者 |
| | `void setMemoryManager(MemoryManager* manager)` | 设置MemoryManager实例 |
| | `void initializePromptFile()` | 初始化提示词文件 |
| | `QString loadSystemPrompt()` | 从文件加载系统提示词 |
| | `void setApiKey(const QString& apiKey)` | 设置API Key |
| **信号** | `void sentencesReady(const QList<SentenceText>& sentences, const QString& rawReply)` | 句子解析完成（复数形式，语义正确） |
| | `void internetErrorSignal(const QString& errorMessage)` | 网络错误 |
| | `void memoryExtractionReady(const QJsonObject& extractionResult, qlonglong lastEndId, const QString& sourceIdsJson, const RequestMeta& meta)` | 记忆提取完成（四类字段；meta 为本轮渠道/会话/说话人/主体） |

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

**API请求参数**：
```cpp
rootObj["model"] = "deepseek-v4-flash";
rootObj["messages"] = messagesArray;
rootObj["temperature"] = 0.7;
rootObj["max_tokens"] = 8192;
rootObj["response_format"] = QJsonObject{{"type", "json_object"}};
```

### TagValidator（标签校验类）

| 类型 | 名称 | 说明 |
|------|------|------|
| **职责** | 标签合法性校验与修正 | - |
| **校验流程** | 精确匹配 → 大小写忽略 → 编辑距离 → 继承上一状态 → 使用默认值 | 5级回退 |
| **关键方法** | `int levenshteinDistance(const QString& s1, const QString& s2)` | 计算编辑距离 |
| | `QString validateTag(const QString& tagName, const QString& rawValue, const QStringList& validValues, const QString& contextText = "", const QString& prevValue = "")` | 校验并修正标签值 |
| | `int getEditDistanceThreshold(const QString& value)` | 获取编辑距离阈值 |

**合法标签值**：
| 标签 | 合法值列表 |
|------|------------|
| emotion | happyIdle, happyMore, amazing, loving, caring, sad, conscientious |
| blush | unblushing, blushing |
| distance | far, closer |
| clothing | pajama, schoolUniform, schoolUniformWithoutCap, schoolUniformWithoutCoat |

### TTSService

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_ttsQueue` | QQueue\<SentenceText\>，合成队列 |
| | `m_isSynthesizing` | bool，合成状态锁 |
| | `m_provider` | ITTSProvider*，TTS Provider 接口 |
| | `m_audioSink` | IAudioSink*，渠道音频输出（非所有权） |
| **函数签名** | `void enqueueSentences(const QList<SentenceText>& sentences)` | 入队多句话 |
| | `void setAudioSink(IAudioSink* sink)` | 注入桌宠音频输出；为空则只合成不播放 |
| | `void processTtsQueue()` | 合成生产者（流式把 PCM 交给 sink） |
| | `void onTtsFinished(...)` | 合成完成：`endStream` / `playFile`，继续合成 |
| | `void onTtsFailed(...)` | 合成失败：中止流式缓冲 + 模拟播放 |
| | `void reloadProvider()` | 根据配置重新加载 TTS Provider |
| | `void switchModel(const QString& gptPath, const QString& sovitsPath)` | 切换 GPT-SoVITS 模型 |
| **信号** | `void playAudioAction(const QString& zhText, const QMap<QString,QString>& tags)` | sink `sentenceStarted` 转发（同步气泡/立绘） |
| | `void playbackQueueEmpty()` | 合成队列空且 sink 空闲 |

### IAudioSink / PetAudioSink（音频输出，批次 C）

| 类型 | 名称 | 说明 |
|------|------|------|
| **接口** | `IAudioSink` | `beginStream/writePcm/updateSampleRate/endStream/abortPendingStream/playFile/isIdle/stop` + `sentenceStarted` / `idle` |
| **实现** | `PetAudioSink` | 桌宠渠道音频输出：拥有 `IPcmPlayer` 与播放队列；首句首块起播、后续句预缓冲、Mock 模拟 |
| **装配** | `PetChannelAdapter::audioSink()` | 桌宠渠道创建并持有；AppController 注入 `TTSService::setAudioSink()` |

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
    virtual void setSynthesisDone() {}  // 流式专用
    virtual void setSource(const QString &path) {}  // 文件型专用
    virtual void updateSampleRate(int sampleRate) {}  // 采样率校正（流式播放器重写）
signals:
    void PcmPlayerFinished();
    void PcmPlayerError(const QString &error);
};
```

**采样率自适应**：
- **流式模式**：ApiTTSProvider 在 readyRead 跳过头时，从服务端 WAV 头偏移 24 读取真实采样率写入 `m_sampleRate`；TTSService 统一经私有 helper `apiSampleRate()`（内部 qobject_cast<ApiTTSProvider*>）取值传给 `startPlayer()`，首块到达时 StreamPlayer 再通过 `updateSampleRate()` 校正（与默认相同则空操作），彻底替代硬编码 24000Hz
- **非流式模式**：FilePlayer 已具备 WAV 头解析逻辑，会从文件头读取采样率并自动重建 QAudioSink；非流式分段响应（isSegmentedResponse）由 ApiTTSProvider 从首个 WAV 头读取采样率后写临时文件
- **采样率取值**：不再按 `super_sampling` 猜测，统一以服务端 WAV 头为准（v3=24000Hz，v1/v2=32000Hz，v4/超分=48000Hz），`m_sampleRate` 默认 24000 仅为兜底

**实现类**：
- `ApiTTSProvider`：调用 GPT-SoVITS HTTP API，streaming_mode=true 流式合成；`m_sampleRate`（默认 24000）+ `getSampleRate()` getter；流式/分段路径均从服务端 WAV 头读取真实采样率写入 `m_sampleRate`，不再按 `super_sampling` 猜测
- `StreamPlayer`：流式PCM播放（QAudioSink + QBuffer，预填充+兜底检测；`updateSampleRate()` 支持采样率变化重建）
- `FilePlayer`：文件型WAV播放（QAudioSink + QFile，WAV头解析+预填充）

### StreamPlayer（流式PCM播放器）

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_audioSink` | QAudioSink*，音频输出 |
| | `m_buffer` | QBuffer*，PCM数据缓冲（初始化nullptr防野指针） |
| | `m_queue` | QQueue\<QByteArray\>，待写入的PCM数据队列 |
| | `m_mutex` | QMutex，保护m_queue |
| | `m_timer` | QTimer*，20ms间隔的pushData定时器 |
| | `m_endTimer` | QTimer*，500ms间隔的播放完成兜底检测定时器 |
| | `m_isSynthesisDone` | bool，合成完成标记 |
| | `m_lastProcessedUsecs` | qint64，上次检测时的已播放微秒数 |
| | `m_endCheckCount` | int，连续满足完成条件的次数 |
| | `m_sampleRate` | int，当前QAudioSink配置的采样率（判断是否需要重建） |
| **函数签名** | `void startPlayer(int sampleRate, int channels, int sampleBits)` | 启动播放器（预填充+QAudioSink::start） |
| | `void stopPlayer()` | 停止播放器，清理资源 |
| | `void writePcm(const QByteArray &pcmData)` | 写入PCM数据到队列 |
| | `void setSynthesisDone()` | 标记合成完成，开启播放完成检测 |
| | `void updateSampleRate(int sampleRate)` | 采样率校正：与当前不同则重建QAudioSink |
| | `void pushData()` | 20ms定时器回调：从队列取数据写入m_buffer |
| | `void checkPlayEnd()` | 500ms定时器回调：兜底检测播放完成 |
| | `void finishAndEmit()` | 统一停止定时器+emit PcmPlayerFinished |

### FilePlayer（文件型WAV播放器）

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_audioSink` | QAudioSink*，音频输出 |
| | `m_buffer` | QBuffer*，PCM数据缓冲 |
| | `m_file` | QFile，WAV文件 |
| | `m_timer` | QTimer*，pushData定时器 |
| | `m_sampleRate` | int，WAV采样率 |
| | `m_channels` | int，WAV通道数 |
| | `m_bitsPerSample` | int，WAV位深 |
| **函数签名** | `void startPlayer(int sampleRate, int channels, int sampleBits)` | 启动播放器 |
| | `void setSource(const QString &filePath)` | 打开WAV文件，解析头，预填充（override IPcmPlayer） |
| | `void pushData()` | 定时器回调：从文件读取数据写入m_buffer（seek到末尾追加） |
| | `void stopPlayer()` | 停止播放，清理资源 |

### TimeManager（时间管理器，独立子类）

| 类型 | 名称 | 说明 |
|------|------|------|
| **职责** | 时间驱动状态管理：表情/脸红/距离退火倒计时、服装时段切换 | - |
| **私有变量** | `m_blushResetTimer` | QTimer*，脸红退火倒计时（singleShot） |
| | `m_emotionResetTimer` | QTimer*，表情退火倒计时（singleShot） |
| | `m_distanceResetTimer` | QTimer*，距离退火倒计时（singleShot） |
| | `m_BackIdleTime` | int，表情退火时间（秒，配置驱动） |
| | `m_blushTime` | int，脸红退火时间（秒，配置驱动） |
| | `m_distanceTime` | int，距离退火时间（秒，配置驱动，默认 15） |
| **函数签名** | `void notifyRoundEnded(finalTags)` | 每轮结束：停全部退火，再按最终状态启动对应定时器（blush=blushing / emotion!=happyIdle / distance=closer） |
| | `void notifyUserInputStarted()` | stop 三个退火定时器（用户输入打断退火） |
| | `void setDistanceTime(int)` | 设置距离退火时长（秒，与 setBlushTime/setBackIdleTime 同格式） |
| | `void onMinuteTick()` | 每分钟检查服装时段切换（白天校服 / 夜晚睡衣） |
| **设计要点** | QTimer::singleShot 替代原 QElapsedTimer 状态机，避免野指针崩溃（TD-030）；退火时间单位修正 hasExpired(15) 误为 15ms → start(15000) 正确为 15秒（TD-032）；退火由「每轮最终状态」驱动，删除 notifyLLMEnded/notifyLLMtagsApplicated/notifyBlushingApplicated 三方法统一为 notifyRoundEnded；昼夜跨零点判断 `isNight = (now >= 22:00 || now < 7:00)`（TD-035，修复原写法把白天误判为夜） | - |

### TTSProcessManager（GPT-SoVITS 进程管理）

| 类型 | 名称 | 说明 |
|------|------|------|
| **职责** | 管理 GPT-SoVITS python 进程的生命周期，清理孤儿进程 | - |
| **函数签名** | `void apiStart()` | 启动 API（开头 TCP 探测端口占用 → killProcessOnPort 清理孤儿） |
| | `void killProcessOnPort(int port)` | 清理占用指定端口的进程（Windows: netstat+taskkill） |
| **设计要点** | 修复 SoVITS 切换 400 Bad Request（端口被占用导致复用坏实例，TD-031）；修复崩溃后留孤儿 python.exe（异常退出端口未释放） | - |

### Utterance / RequestMeta（Phase 0 统一协议）

| 类型 | 名称 | 说明 |
|------|------|------|
| **Utterance（输出）** | `text` / `ttsText` / `tags` / `channel` / `sessionId` / `speaker` | 渠道无关的统一输出单元（由 SentenceText 映射） |
| **RequestMeta（输入）** | `channel` / `sessionId` / `speaker` / `subject` / `sensitive` | 随轮次贯穿入库、摘要分桶、关系更新 |
| **Channel 常量** | `kPet="pet"` / `kQQPrivate="qq_private"` / `kQQGroup="qq_group"` / `kQZone="qzone"` / `kDefaultSession="default"` | 与 DB channel 列、盲区配置取值一致 |

### IChannelAdapter（渠道抽象）

| 类型 | 名称 | 说明 |
|------|------|------|
| **接口** | `channelName()` / `sendUtterance(const Utterance&)` | 纯虚接口，大脑侧不感知具体渠道 |
| **PetChannelAdapter** | 持 `BubbleWidget*`（非所有权） | `sendUtterance` → `showMessage`；持有 `PetAudioSink`（`audioSink()`），播放器归桌宠渠道 |
| **QQChannelAdapter** | QQ 文本输出 | `sendUtterance` → `send_private_msg`/`send_group_msg`；依赖 `QQProtocolClient` |

### QQ 渠道（文本已接入）

| 类 | 职责 | 关键点 |
|----|------|--------|
| `QQProtocolClient` | OneBot v11 WS 客户端 | 连接 `ws://127.0.0.1:3001`；Bearer token；3s→30s 重连；`get_login_info` 自检；发送 `retcode=100` 重试一次 |
| `QQMessageParser` | 入站事件解析 | `post_type=message`、private/group、speaker（card→nickname）、`mentionedSelf`、text/at/image/record/face 占位、原始 JSON |
| `QQReplyPolicyEngine` | 回复策略 | `private: none/master/allowlist/all`；`group: none/mention/all`；`group_allow`；`master_any_group`；Drop/RecordOnly/Reply |
| `QQChannelAdapter` | 文本出站 | 文本消息段数组；群/私聊动作选择；未连接只打日志 |

### SessionRouter（输入路由）

| 类型 | 名称 | 说明 |
|------|------|------|
| **职责** | 输入归一化 → 身份解析 → 按会话桶组装历史 → 发 `requestReply` | - |
| **函数** | `bool submitInput(channel, sessionId, speaker, text, senderId = QString())` | 空输入/自身消息返回 false；组装 `RequestMeta` |
| **函数** | `QString resolveSubject(channel, senderId)` | 桌宠/空→`user`；self→空串（丢弃）；master→`user`；未知 QQ→`qq:<uin>`；其他→`<channel>:<id>` |
| **构造** | `SessionRouter(...)` | 启动时调用 `ConfigManager::loadIdentityConfig()` |
| **信号** | `void requestReply(const QString&, const QList<HistoryTurn>&, const RequestMeta&)` | AppController 接收后写 `m_lastMeta` 并调用 askDeepSeek |

### PromptAssembler（上下文组装）

| 类型 | 名称 | 说明 |
|------|------|------|
| **结构** | `MemoryContext` | userProfiles / makoProfiles / memories / summaries / relationships |
| **函数** | `buildSystemPrompt(base, meta, context)` | 档案/情景/摘要/关系按渠道过滤后拼 system prompt（C3） |
| **函数** | `buildHistoryMessages(history, meta)` | 短期记忆 messages；proactive/perception assistant-only；群聊说话人前缀 |
| **函数** | `isEpisodicVisible()` / `isUserProfileVisible()` | 可见性规则：pet 全可见、public 全渠道、channel 需 origin+session、private 仅 pet；群聊过滤亲密档案 key |

### GlobalMoodState（全局情绪，批次 D）

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_emotion` / `m_blush` / `m_distance` | 全局情绪三维（默认 happyIdle / unblushing / far） |
| | `m_followExternalMood` | D1 开关（默认 true，来自 identity.json） |
| **函数** | `applyTags(tags, sourceChannel)` | 更新全局情绪；非桌宠且 D1 开启时发 `externalMoodApplied` |
| | `setFollowExternalMood(bool)` / `followExternalMood()` | 设置/读取 D1 |
| | `getCurrentStateDescription(clothing)` | 与 AppearanceManager 同格式的状态描述 |
| **信号** | `externalMoodApplied(const QMap<QString,QString>&)` | 外部情绪驱动桌宠立绘（仅情绪三维，不含服装） |

### MemoryManager（AI记忆系统）

| 类型 | 名称 | 说明 |
|------|------|------|
| **职责** | 管理原始事件流水、对话轮次、角色档案、情景记忆、长期摘要和关系状态 | - |
| **私有变量** | `m_db` | QSqlDatabase，SQLite数据库连接 |
| **函数签名** | `bool saveQATurn(const QString& userInput, const QString& rawReply, const QList<SentenceText>& sentences, const RequestMeta& meta = RequestMeta())` | 保存单轮对话（raw_reply为JSON对象；meta 写入 channel/session/speaker/sensitive） |
| | `QList<HistoryTurn> getHistoryTurn(int N, const QString& channel = "pet", const QString& sessionId = "default")` | 按渠道+会话双键获取最近N轮对话历史 |
| | `QList<HistoryTurn> getHistoryTurn(int offset, int limit)` | 分页获取对话历史 |
| | `qlonglong getTotalHistoryCount()` | 获取历史记录总数 |
| | `bool deleteTurnByID(int id)` | 根据ID删除单条记录 |
| | `bool clearAllHistory()` | 清空所有历史记录 |
| | `bool appendRawEvent(platform, channel, sessionId, eventType, content, messageId, direction, senderId, senderName, payloadJson, occurredAt, sensitive, visibility)` | 写入 raw_events（`INSERT OR IGNORE`，message_id 去重；本阶段不接线） |
| | `bool updateMemoryCursor(channel, sessionId, lastEventId, lastTurnId)` | 更新蒸馏游标 |
| | `qlonglong getMemoryCursorEventId(channel, sessionId)` | 读取事件游标（无记录返回 0） |
| | `QList<HistoryTurn> getUnsummarizedTurns(qlonglong& outLastEndId, QList<qlonglong>& outSourceIds, const QString& channel = "pet", const QString& sessionId = "default")` | 按双键获取未摘要对话（游标子查询同样按双键） |
| | `bool upsertCharacterProfile(const QString& subject, const QString& key, const QString& value)` | 插入或更新角色档案（先 normalizeProfileKey 归一化 key，已存在则 mergeProfileValue 合并 value） |
| | `QList<CharacterProfile> getCharacterProfiles(const QString& subject)` | 获取档案（subject 为空时返回全部） |
| | `bool deleteCharacterProfile(qlonglong id)` | 删除档案 |
| | `bool addEpisodicMemory(content, type, importance, eventTime = QDateTime(), sourceIds = QString(), origin = "pet", visibility = "private", subject = "user", sessionId = "default")` | 新增情景记忆（含渠道溯源/可见性/主体/会话） |
| | `QList<EpisodicMemory> getActiveEpisodicMemories(double minImportance = 0.2, int limit = 20, const QString& origin = QString(), const QString& visibility = QString(), const QString& sessionId = QString())` | 获取活跃记忆（status='active' AND importance≥阈值，按 importance DESC 排序，更新 last_accessed；可按 origin/visibility/sessionId 过滤） |
| | `bool deleteEpisodicMemory(qlonglong id)` | 删除记忆 |
| | `bool updateEpisodicMemoryStatus(qlonglong id, const QString& status)` | 更新记忆状态（promise 兑现→resolved / 失约→broken） |
| | `int decayEpisodicMemory()` | 衰减扫描：importance≥0.8 不衰减；其余 -0.05/天；<0.1 删除（基于 last_decay_at） |
| | `QString normalizeProfileKey(const QString& subject, const QString& rawKey)` | 同 subject 内基于编辑距离归一化 key，匹配已有同义 key |
| | `QString mergeProfileValue(const QString& key, const QString& oldVal, const QString& newVal)` | 合并 value（newVal 非空且更长则采纳 newVal，否则保留 oldVal） |
| | `int levenshteinDistance(const QString& s1, const QString& s2)` | 计算两个字符串的 Levenshtein 编辑距离 |
| | `int getEditDistanceThreshold(const QString& value)` | 根据 value 长度动态返回编辑距离阈值（≤4字=1，≤8字=2，≤12字=3，>12字=4） |
| | `bool addLongTermSummary(const QString& summaryText, qlonglong coveredEndId, const QString& sourceIdsJson, const QString& channel = "pet", const QString& sessionId = "default")` | 添加长期摘要（按双键分桶） |
| | `QList<LongTermSummary> getLatestSummaries(int limit = 5, const QString& channel = "pet", const QString& sessionId = "default")` | 获取最新摘要（is_dirty=0，按双键过滤） |
| | `bool deleteLongTermSummary(qlonglong id)` | 删除摘要 |
| | `bool upsertRelationshipState(const QString& dimension, double delta, const QString& subject = "user")` | 按 subject 维度不存在时初始化为 clamp(30+delta,0,100)；存在时 clamp(oldVal+delta,0,100) |
| | `QList<RelationshipState> getRelationshipStates(const QString& subject = "user")` | 获取指定主体的全部关系维度（按 dimension 排序） |
| | `bool initRelationshipState()` | 若表为空，插入 intimacy=30、trust=30 |
| | `void initDatabase(const QString& dbFilePath)` | 初始化数据库：v1 基础表 → v2 多渠道 → v3 记忆库调整 → v4 会话隔离（`user_version=4`，7 张表） |
| **数据库路径** | `app_data/memory/QianDaoMoZi_memory.db` | - |

**数据库结构**（`PRAGMA user_version = 4`；v1/v2 基础 + v3/v4 记忆库调整，合计 7 张表）：

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
| session_id | TEXT NOT NULL DEFAULT 'default' | 会话（v4；QQ 群号/好友号，桌宠 default） |

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

### 数据结构

**HistoryTurn（对话历史）**：
```cpp
struct HistoryTurn {
    qlonglong id = -1;
    QDateTime timestamp;
    QString userInput;  // 主动/感知轮次为 SQL NULL（读取后为空串）
    QString rawReply;   // JSON对象: {"sentences": [...]}
    QString turnKind = "reply";  // reply / proactive / perception
    QString subject = "user";    // user / qq:<uin> / group:<群号>
    QString speaker;               // 说话人显示名（群聊历史注入）
};
```

**CharacterProfile（角色档案，长期稳定特质，user 或 mako）**：
```cpp
struct CharacterProfile {
    qlonglong id = -1;
    QString subject;    // 'user' 或 'mako'
    QString key;        // 如 'nickname'/'occupation'/'persona'
    QString value;
    QDateTime updatedAt;
};
```

**EpisodicMemory（情景记忆，事件/承诺/冲突/里程碑）**：
```cpp
struct EpisodicMemory {
    qlonglong id = -1;
    QString content;    // 事件描述
    QDateTime eventTime;
    double importance = 0.5;   // 0.0-1.0
    QString type;       // 'event'/'promise'/'conflict'/'milestone'
    QString status;     // 'active'/'resolved'/'broken'
    QDateTime lastAccessed;
    QString sourceIds;
    QString origin = "pet";          // 渠道溯源
    QString visibility = "private";  // private / channel / public
    QString subject = "user";        // 记忆所属主体
    QString sessionId = "default";  // 会话（QQ 群号/好友号）
};
```

**LongTermSummary（长期记忆摘要）**：
```cpp
struct LongTermSummary {
    qlonglong id = -1;
    QString summaryText;
    qlonglong coveredTurnEndId = -1;
    QString sourceIds;
    bool isDirty = false;
    QDateTime createdAt;
    QDateTime updatedAt;
};
```

**RelationshipState（关系状态，量化 user 与 AI 之间的关系维度）**：
```cpp
struct RelationshipState {
    QString dimension;   // 'intimacy'(亲密度) / 'trust'(信任度)
    double value = 0.0;  // 0-100
    QDateTime updatedAt;
};
```

**SentenceText（句子数据）**：
```cpp
struct SentenceText {
    QString zhText;                  // 中文文本 → BubbleWidget显示
    QString jaText;                  // 日文文本 → TTS合成输入
    QMap<QString, QString> rawTags;  // 四维状态标签 → AppearanceManager换图
    bool isValidated;                // 标签是否已校验
};
```

### BubbleWidget

| 类型 | 名称 | 说明 |
|------|------|------|
| **私有变量** | `m_timer` | QTimer*，打字机定时器 |
| | `m_text` | QString，完整文本 |
| | `m_idex` | int，当前显示位置 |
| **函数签名** | `void showMessage(const QString& text)` | 显示气泡并启动打字机 |
| | `void typeWriteEffect()` | 打字机特效 |
| **信号** | `void bubbleShown()` | 气泡显示时发出，通知位置管理器 |

### ConfigManager

| 类型 | 名称 | 说明 |
|------|------|------|
| **模式** | Meyers单例（线程安全） | - |
| **私有变量** | `m_apiKey` | QString，DeepSeek API Key |
| | `m_ttsUrl` | QString，GPT-SoVITS服务地址 |
| | `m_configDirPath` | QString，配置目录路径 |
| | `m_configFilePath` | QString，配置文件路径 |
| **函数签名** | `static ConfigManager& instance()` | 获取单例 |
| | `QString getApiKey() const` | 获取API Key |
| | `void setApiKey(const QString& apiKey)` | 设置API Key |
| | `QString getTTSUrl() const` | 获取TTS服务地址 |
| | `void setTTSUrl(const QString& ttsUrl)` | 设置TTS服务地址 |
| | `int getShortMemoryLength() const` | 获取短期记忆长度 |
| | `QString getConfigDirPath() const` | 获取配置目录路径 |
| | `QString getMemoryPath() const` | 获取数据库文件路径 |
| | `int getSummaryMemoryLength() const` / `setSummaryMemoryLength(int)` | 长期记忆自动摘轮数（TD-037） |
| | `QTime getNightStartTime() const` / `setNightStartTime(QTime)` | 夜晚开始时间 |
| | `QTime getNightEndTime() const` / `setNightEndTime(QTime)` | 夜晚结束时间 |
| | `int getBlushLasting() const` / `setBlushLasting(int)` | 脸红退火时长（秒） |
| | `int getEmotionLasting() const` / `setEmotionLasting(int)` | 表情退火时长（秒） |
| | `int getDistanceLasting() const` / `setDistanceLasting(int)` | 距离退火时长（秒） |
| | `bool loadSetting()` | 从JSON文件加载配置 |
| | `bool saveSetting()` | 保存配置到JSON文件 |
| | `void loadIdentityConfig()` / `reloadIdentity()` | 显式加载/重载 identity.json（启动时由 SessionRouter 触发） |

> ✅ 配置文件持久化已实现：`app_data/config/setting.json`

**QQ 配置（`setting.json` 的 `qq` 段）**：

| 类型 | 名称 | 说明 |
|------|------|------|
| **结构** | `QQConfig` | `enabled` / `wsUrl`（默认 `ws://127.0.0.1:3001`）/ `accessToken` |
| | `QQReplyPolicy` | `privateMode` / `groupMode` / `privateAllow` / `groupAllow` / `masterAnyGroup` |
| **函数** | `QQConfig getQQConfig() const` | 读取 QQ 连接与回复策略 |

**Phase 0 扩展（`app_data/config/identity.json`，懒加载、只读解析、缺失用默认值）**：

| 类型 | 名称 | 说明 |
|------|------|------|
| **结构** | `BlindZone` | 记忆盲区（channel + sessionId） |
| | `ScheduleItem` | 主动对话时刻表项（at + type: fixed/random） |
| | `ProactiveConfig` | enabled/targets/dailyLimit/cooldownSeconds/quietStart/quietEnd/idleSeconds/schedule |
| **函数** | `QStringList getSelfQQAccounts()` / `getMasterQQAccounts()` | 她自己的 QQ / 主人 QQ（多账号均归 `subject='user'`） |
| | `bool isMasterQQ(const QString& uin)` | 按 uin 精确匹配 |
| | `bool isBlindZone(channel, sessionId)` | 双键查盲区 |
| | `bool getFollowExternalMood()` | D1：外部渠道情绪是否驱动桌设立绘（默认 true） |
| | `ProactiveConfig getProactiveConfig()` | Phase 0 只解析，Phase 2 接入调度 |

### SettingsWidget（设置界面）

> v0.6.0 重构：删除原多标签页实现，改为「左侧目录 + 右侧滚动内容」的主-从式布局，支持双向 scroll-spy 同步。

| 类型 | 名称 | 说明 |
|------|------|------|
| **职责** | 设置界面，管理API Key、TTS模型切换、记忆长度配置和对话历史管理 | - |
| **配置项** | API Key配置、TTS模型配置（GPT路径/模型、SoVITS路径/模型，保存时热切换）、短期记忆轮数、历史记录管理 | - |
| **布局** | 左侧目录树 `treeIndex` + 右侧滚动区 `scrollArea`；一级区域 `section1_*` + 二级分区 `section2_*` | - |
| **双向同步** | 点击目录→滚到分区（`ensureWidgetVisible`）；滚动→按页面位置高亮目录项（`m_sectionMap` + `m_syncGuard`） | - |
| **滚轮透传** | 下拉框/微调框/时间编辑框 `eventFilter`，滚轮转给右侧滚动条不拦截界面滚动 | - |
| **已接线** | 长期记忆摘轮数（`memory.summary_length`）、夜晚时段与退火时长（`time.*`），保存后通过 `settingsSaved` 热应用（TD-037） | - |
| **待完善** | 外观/快捷键配置尚未实现 | - |
| **函数签名** | `void show()` | 显示设置窗口 |
| | `void setMemoryManager(MemoryManager* manager)` | 注入MemoryManager实例 |
| | `void setMemoryLength(int length)` | 初始化记忆长度设置 |
| | `void buildSync()` | 建立目录↔分区映射并连接双向同步 |
| | `void refreshHistoryTurnList()` | 刷新历史记录列表 |
| | `void loadHistoryPage(int page)` | 加载指定页的历史记录 |
| | `bool eventFilter(QObject*, QEvent*)` | 滚轮透传 |
| | `void on_btn_saveApiKey_clicked()` | 保存API Key |
| | `void on_btn_deleteSelectedMemory_clicked()` | 删除选中的历史记录 |
| | `void on_btn_claenAllMemory_clicked()` | 清空所有历史记录 |
| **信号** | `void settingsSaved()` | 设置保存后发出 |
| | `void ttsModelSwitchRequested(gptPath, sovitsPath)` | GPT/SoVITS模型保存成功时发出，通知TTS热切换 |

---

## 2. 关键通信逻辑

### AI对话完整流程（含记忆系统）

```
用户右键 → chatRequested信号 → ChatWidget::popup() → 用户输入 → textSubmitted信号
                                                                    │
                                                                    ▼
                                                      AppController lambda (保存m_lastUserInput)
                                                                    │
                                                                    ▼
                                                      MemoryManager::getHistoryTurn(15) 获取短期记忆
                                                                    │
                                                                    ▼
                                                      LLMService::askDeepSeek(userInput, historyQA)
                                                                    │
                                                                    ▼
                                                             DeepSeek API (JSON Mode)
                                                                    │
                                                                    ▼
                                                          onReplyFinished 解析JSON
                                                                    │
                                                         TagValidator校验标签
                                                                    │
                                                                    ▼
                                                      sentencesReady(QList<SentenceText>, rawReply)
                                                                    │
                                                                    ▼
                                             AppController::handleMakoReply
                                               ┌────────────────┴────────────────┐
                                               ▼                                 ▼
                                        MemoryManager::saveQATurn        TTSService::enqueueSentences
                                        (保存用户输入+JSON回复)            │
                                                          ┌───────────┴───────────┐
                                                          ▼                       ▼
                                                   合成队列(Producer)       播放队列(Consumer)
                                                          │                       │
                                                          ▼                       ▼
                                                   ApiTTSProvider        playAudioAction信号
                                                   (GPT-SoVITS HTTP)              │
                                                                                        │
                                                                                        ▼
                                                                  AppController::onPlayAudioAction
                                                                       ┌──────┴──────┐
                                                                       ▼             ▼
                                                               气泡显示中文    立绘切换状态
```

> **Phase 0 变更**：对话流程第 3 步已由 `SessionRouter` 接管——`textSubmitted → submitInput("pet","default","欧尼酱",text)`，路由层完成身份解析与 `getHistoryTurn(N, channel, sessionId)` 双键历史组装后，再以 `requestReply(text, history, meta)` 交回 AppController。

### 信号槽连接（AppController中统一管理）

| 发送方 | 信号 | 接收方 | 槽/处理 |
|--------|------|--------|---------|
| CharacterWidget | `chatRequested()` | ChatWidget | `popup()` |
| CharacterWidget | `settingsRequested()` | SettingsWidget | `show()` |
| ChatWidget | `textSubmitted(text)` | SessionRouter | `submitInput("pet","default","欧尼酱",text)`（身份解析+双键历史） |
| SessionRouter | `requestReply(text, history, meta)` | AppController lambda | 写 `m_lastUserInput`/`m_lastMeta` → `notifyUserInputStarted` → `askDeepSeek` |
| LLMService | `sentencesReady(sentences, rawReply)` | AppController | `handleMakoReply()` |
| LLMService | `internetErrorSignal(msg)` | AppController | `handleSystemError()` |
| LLMService | `memoryExtractionReady(extractionResult, lastEndId, sourceIdsJson, meta)` | AppController lambda | 按本轮 meta 写入档案+情景记忆+摘要+关系状态 |
| TTSService | `playAudioAction(zhText, tags)` | AppController | `onPlayAudioAction()` |
| AppearanceManager | `characterPathChanged(path)` | CharacterWidget | `updatePath()` |
| AppearanceManager | `characterPathChanged(path)` | AnchorManager | `updateAllAnchors()` |
| BubbleWidget | `bubbleShown()` | AnchorManager | `updateAllAnchors()` |
| SettingsWidget | `settingsSaved()` | AppController lambda | 更新LLMService API Key |
| SettingsWidget | `ttsModelSwitchRequested(gptPath, sovitsPath)` | AppController lambda | 调用TTSService::switchModel热切换GPT/SoVITS模型 |

### AI状态同步机制（方案C · 已实现）

| 特性 | 说明 |
|------|------|
| **策略** | 本地切换后，下轮对话时将状态追加到系统提示词 |
| **状态内容** | 服装、心情、距离、脸红、时间 |
| **同步时机** | 每次调用askDeepSeek()前，动态获取并追加 |
| **优势** | 不额外消耗token，AI自动感知状态变化 |
| **实现方式** | 通过 `registerStateProvider()` 注册回调 |

### AI记忆系统（已实现·五层记忆架构）

| 层级 | 说明 | 存储位置 |
|------|------|----------|
| **短期记忆** | 最近N轮对话历史（默认15轮） | chat_history表 |
| **角色档案** | user/mako 长期稳定特质（subject/key/value，无置信度无 tier） | character_profile表 |
| **情景记忆** | 带时间戳的共同事件（event/promise/conflict/milestone），importance 衰减 | episodic_memory表 |
| **长期摘要** | 重要对话的摘要总结 | long_term_summary表 |
| **关系状态** | 量化 user 与 AI 关系维度（intimacy/trust，0-100，默认30） | relationship_state表 |

**历史记忆注入**：只注入纯文本，剥离标签格式，避免影响LLM输出格式。

**askDeepSeek 系统提示词注入顺序**：基础prompt → 角色档案(mako→`# 茧子的人设`，user→`# 关于欧尼酱`) → 情景记忆(`# 我们的回忆`) → 长期摘要(`# 最近发生`) → 关系状态(`# 我们的关系`，渲染为 `亲密度: X/100`、`信任度: X/100`) → 短期记忆 → 当前环境状态 → 用户输入

**extractMemoryAsync 异步记忆提取**：传入已有角色档案+情景记忆+当前关系状态做增量更新，LLM 输出 JSON 含 `character_updates`/`episodic_memories`/`working_summary`/`relationship_updates` 四类字段，经 `memoryExtractionReady` 信号回调写入对应表

---

## 3. 现有痛点/未解逻辑

### 模拟/占位功能

| 位置 | 说明 |
|------|------|
| TTSService | MockTTSProvider为模拟实现，ApiTTSProvider需GPT-SoVITS服务运行 |

### 待开发功能

- [x] 配置文件持久化 ✅ 已实现（JSON文件）
- [x] AI记忆系统（短期记忆）✅ 已实现（SQLite数据库）
- [x] AI记忆系统（中期/长期记忆）✅ 已实现（角色档案+情景记忆+长期摘要+关系状态）
- [x] 系统提示词外部化 ✅ 已实现（prompt.txt文件）
- [x] AI状态同步机制 ✅ 已实现（状态提供者模式）
- [x] JSON输出协议 ✅ 已实现（response_format+JSON解析）
- [x] 标签校验 ✅ 已实现（TagValidator+编辑距离）
- [x] 流式TTS播放 ✅ 已实现（streaming_mode + pcmDataReady）
- [x] IPcmPlayer播放器抽象 ✅ 已实现（StreamPlayer + FilePlayer）
- [x] 播放完成检测 ✅ 已实现（setSynthesisDone + 兜底定时器）
- [x] 预填充防吞开头 ✅ 已实现（startPlayer预填充PCM）
- [x] 播放器工厂模式重构 ✅ 已实现（IPcmPlayer 静态工厂方法）
- [x] TimeManager 时间管理器 ✅ 已实现（QTimer singleShot 退火、服装时段切换）
- [x] TTSProcessManager 孤儿进程清理 ✅ 已实现（killProcessOnPort）
- [x] 设置界面重构 ✅ 已实现（左侧目录+右侧滚动主从布局，双向 scroll-spy 同步，滚轮透传）
- [x] TTS模型切换 ✅ 已实现（设置界面「语音合成」区：GPT路径/模型、SoVITS路径/模型，保存时热切换）
- [x] 多渠道架构（Phase 0）✅ 已实现（Utterance/IChannelAdapter/SessionRouter、DB v2、identity.json、23 例测试）
- [x] 异常回复拦截 ✅ 已实现（zh_text/ja_text/tags 缺失整轮抛弃，replyAbnormal 反馈）
- [x] Phase 0.5 / 记忆库 v3 ✅ 已实现（`raw_events`、轮次语义、`visibility`/`subject`、`memory_cursor`；提交 8f24ae5）
- [ ] Phase 1 QQ 渠道（SnowLuma / OneBot v11）
- [x] 设置界面接线（TD-037：长期记忆摘轮数、夜晚时段、三档退火时长）✅ 已实现
- [ ] 设置界面完善（外观配置、快捷键配置）
- [ ] TTS语速、温度配置
- [ ] 立绘切换过渡动画
- [ ] 错误重试机制（LLM请求失败自动重试）

### 已知代码问题

| 位置 | 问题描述 | 严重程度 |
|------|----------|----------|
| anchormanager.cpp | ~~AnchorManager析构未清理m_anchors（内存泄漏风险）~~（TD-014 已结案：仅持弱引用，widget 所有权归 AppController） | ~~高~~ |
| appcontroller.cpp | ~~SUMMARY_THRESHOLD魔法数字硬编码~~（TD-016 已取代：阈值改由 `memory.summary_length` 配置驱动，TD-037） | ~~低~~ |
| llmservice.h | ~~头文件依赖memorymanager.h，增加编译依赖链~~（TD-015 已修复：llmservice.h 已用 `class MemoryManager;` 前向声明） | ~~中~~ |
| ttsservice.cpp | ~~直接new StreamPlayer/FilePlayer~~（TD-023 已修复，改用 IPcmPlayer::create 静态工厂） | ~~低~~ |

---

## 4. 项目文件结构

```
Windows_AI/
├── docs/                    # 文档目录
│   ├── project_tech_snapshot.md
│   ├── development_log.md
│   ├── ARCHITECTURE.md
│   └── ROADMAP.md
├── main.cpp                 # 入口函数（创建AppController）
├── CMakeLists.txt           # 构建配置
├── image.qrc                # 资源文件
├── appcontroller.h/cpp      # 应用中枢
├── utterance.h              # 统一协议（Utterance/RequestMeta/Channel）
├── ichanneladapter.h        # 渠道抽象接口
├── petchanneladapter.h/cpp  # 桌宠渠道适配器
├── qqprotocolclient.h/cpp   # OneBot v11 WS 客户端
├── qqmessageparser.h/cpp    # QQ 入站事件解析
├── qqreplypolicy.h/cpp      # QQ 白名单/@ 门控策略
├── qqchanneladapter.h/cpp   # QQ 文本输出
├── sessionrouter.h/cpp      # 输入路由
├── anchormanager.h/cpp      # 位置锚点管理
├── anchorstrategy.h         # 锚点策略
├── chatwidget.h/cpp         # 聊天输入窗口
├── sentencedata.h           # 句子数据结构
├── historyturn.h            # 历史数据结构（HistoryTurn/CharacterProfile/EpisodicMemory/LongTermSummary/RelationshipState）
├── appearancemanager.h/cpp  # 外观管理器
├── ttsservice.h/cpp         # TTS语音服务
├── ittsprovider.h           # TTS Provider接口
├── apittsprovider.h/cpp     # GPT-SoVITS API实现（流式合成）
├── ipcmplayer.h/cpp         # 播放器抽象接口（含静态工厂方法）
├── streamplayer.h/cpp       # 流式PCM播放器
├── fileplayer.h/cpp         # 文件型WAV播放器
├── mockttsprovider.h/cpp     # Mock实现
├── llmservice.h/cpp         # AI服务模块（含TagValidator内部类）
├── configmanager.h/cpp      # 配置管理器
├── memorymanager.h/cpp      # AI记忆系统
├── characterwidget.h/cpp    # 角色主组件
├── bubblewidget.h/cpp/ui    # 气泡组件
├── settingswidget.h/cpp/ui  # 设置界面
├── ttsreference.h           # 参考音频数据结构
├── ttsreferencemanager.h/cpp  # 参考音频情绪映射
├── tests/                   # Phase 0 单元测试（Qt Test + CTest）
└── image/                   # 立绘资源目录
    ├── closer/              # 近景
    ├── far/                 # 远景
    └── default_config/      # 默认配置（prompt.txt）
```

---

## 5. 模块关系图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         AppController                                │
│                    （应用中枢 / 信号调度中心）                          │
│                                                                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
│  │  Character  │  │   Bubble    │  │   Chat      │  │  Settings   │ │
│  │   Widget    │  │   Widget    │  │   Widget    │  │   Widget    │ │
│  │ 立绘/右键菜单 │  │ 气泡/打字机  │  │ 聊天输入框  │  │   设置界面   │ │
│  └─────────────┘  └──────┬──────┘  └──────┬──────┘  └─────────────┘ │
│         │                │                │                         │
│         │                └────────┬───────┘                         │
│         │                         │                                 │
│         ▼                         ▼                                 │
│  ┌─────────────┐         ┌─────────────┐                           │
│  │  Anchor     │         │  LLMService │                           │
│  │   Manager   │         │  AI对话/JSON解析/标签校验                │
│  │ 位置锚点管理 │         └──────┬──────┘                           │
│  └─────────────┘                ▼                                   │
│                          ┌─────────────┐                           │
│                          │ ConfigM-    │                           │
│                          │   anager    │                           │
│                          │  单例配置    │                           │
│                          └─────────────┘                           │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                        TTSService                            │  │
│  │  合成队列 → ITTSProvider(ApiTTSProvider 流式合成)            │  │
│  │  ← pcmDataReady 信号 → StreamPlayer.writePcm()               │  │
│  │  播放队列 → IPcmPlayer(StreamPlayer/FilePlayer)              │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─────────────┐  ┌─────────────┐                                  │
│  │ Appearance  │  │  Memory     │                                  │
│  │   Manager   │  │   Manager   │                                  │
│  │ 四维状态管理 │  │  AI记忆系统  │                                  │
│  └─────────────┘  └─────────────┘                                  │
│                                                                     │
│  核心数据结构：                                                     │
│  SentenceText (zhText + jaText + rawTags)                          │
│  HistoryTurn / CharacterProfile / EpisodicMemory /                 │
│  LongTermSummary / RelationshipState                              │
│  TagValidator (编辑距离标签校验)                                    │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. 近期变更

| 日期 | 变更内容 |
|------|----------|
| 2026-10-27 | QQ 文本接入（M38）：`QQProtocolClient`/`QQMessageParser`/`QQChannelAdapter`；入站 `raw_events` + `SessionRouter`，回复走 QQ 不触发 pet TTS |
| 2026-10-27 | QQ 白名单/@ 门控（M39）：`QQReplyPolicy`（Drop/RecordOnly/Reply）、未通过不记录、通过群未 @ 只记录、`retcode=100` 重试；7 套件 / 64 例 |
| 2026-10-27 | GlobalMoodState（批次 D）：情绪三维提升为全局状态；`applyPetTags()` 统一桌宠立绘+全局情绪；D1 `followExternalMood` 控制外部情绪是否驱动立绘；TimeManager 退火仍归桌宠（D2） |
| 2026-10-27 | TTS 合成/播放分离（批次 C）：新增 `IAudioSink` / `PetAudioSink`；播放器与播放队列归桌宠渠道；TTSService 只合成，无 sink 时只合成不播放；人工听感回归通过 |
| 2026-10-27 | v4 提取链路（提交 `1aa4436`）：`episodic_memory` + session_id；提取 prompt 带渠道/说话人/主体；档案三条准入规则；写入按本轮 meta 归属；信号回传 meta |
| 2026-10-27 | PromptAssembler：上下文组装独立成类，群聊可见性过滤（C3）、说话人前缀、proactive assistant-only |
| 2026-10-27 | identity 接线：`submitInput(senderId)`、`resolveSubject` 查 identity.json、启动时显式加载 |
| 2026-10-27 | 记忆库 v3（提交 `8f24ae5`）：`chat_history` 轮次语义、`raw_events` 泛化、`episodic_memory` + visibility/subject、`memory_cursor`；真实库 v2→v3（107→108 行）迁移验证通过 |
| 2026-10-26 | TD-034 退火轮次门控：`m_roundSeq`/`m_lastPlaybackRound`，`playbackQueueEmpty` 仅在最新一轮生效，旧轮收尾不再打断新一轮 |
| 2026-10-26 | TD-037 设置界面接线：长期记忆摘轮数 + 夜晚时段 + 三档退火时长（ConfigManager 新增 `memory.summary_length`/`time.*` 读写与 6 组 getter/setter；SettingsWidget 三个保存槽；AppController `applyTimeSettings()` 启动与保存时应用；tst_configmanager 新增 3 例） |
| 2026-10-26 | 文档补录：README/ROADMAP/ARCHITECTURE/技术快照/开发记录同步 Phase 0 实际状态（渠道架构、DB v2、23 例测试、BUG-1/BUG-2） |
| 2026-10-20 | Phase 0 验收：人工异常回复校验 5 场景通过；修复 BUG-2 空标签编辑距离误判；全量 23 用例通过 |
| 2026-10-19 | Phase 0 架构解耦落地：Utterance/IChannelAdapter/PetChannelAdapter/QQChannelAdapter(占位)/SessionRouter；数据库 v2 迁移；identity.json 解析；23 例单元测试 |
| 2026-08-23 | 立绘切换修复（TD-036）：`updatePath` 锚定脚底中心 + 原子 setGeometry + repaint + WA_NoSystemBackground，消除 closer 下坠漂移与切换闪帧；昼夜判断跨零点修正（TD-035）：`isNight = (now >= 22:00 || now < 7:00)`，修复白天误判为夜穿睡衣 |
| 2026-08-08 | TimeManager 重构为 QTimer singleShot 退火机制（表情/脸红退火倒计时、服装时段切换），修复野指针崩溃（TD-030，QElapsedTimer 指针未初始化）与退火时间单位错误（TD-032，hasExpired(15) 误为 15ms → start(15000) 为 15秒）；AppController onPlayAudioAction 接入脸红退火触发（blush=blushing 时 notifyBlushingApplicated） |
| 2026-08-08 | TTSProcessManager 孤儿进程清理：apiStart 开头 TCP 探测端口占用 → killProcessOnPort 杀孤儿 python.exe（Windows: netstat+taskkill），修复 SoVITS 切换 400 Bad Request 与崩溃后留孤儿进程（TD-031） |
| 2026-08-08 | 技术债务结案：TD-014 AnchorManager 内存泄漏非问题（仅持弱引用，widget 所有权归 AppController）、TD-015 LLMService 头文件依赖已修复（前向声明）、TD-024/025 随 user_profile 表废弃标记 |
| 2026-08-07 | relationship_state 表创建位置修复：从无条件创建（v1 迁移块外）移入 v1 迁移块作为第 5 张表，统一迁移归属 |
| 2026-08-07 | 关系状态系统完成：relationship_state 表（dimension/value/updated_at，UNIQUE(dimension)），默认 intimacy=30/trust=30；askDeepSeek 注入 `# 我们的关系` 段落（亲密度/信任度 X/100）；extractMemoryAsync 提取 relationship_updates（dimension+delta），经 memoryExtractionReady 写入 |
| 2026-08-07 | WorkingMemory 工作记忆模块曾实现（内存级 currentTopic/userMood/userIntent/contextSummary，零额外API成本，主对话JSON顺带输出），经评估非必要，已彻底移除（llmservice.cpp/h、historyturn.h、prompt.txt 三处清理） |
| 2026-08-07 | prompt.txt 资源默认配置（image/default_config/prompt.txt）从旧标签格式更新为 JSON 协议格式 |
| 2026-08-07 | 数据库架构重整：废弃 user_profile 表（置信度/tier/衰减），改为 character_profile（subject/key/value，无置信度）+ episodic_memory（importance 衰减）+ relationship_state；数据库版本统一为 v1 单一迁移块 |
| 2026-08-05 | 用户画像 key 归一化：新增 normalizeProfileKey()，基于 Levenshtein 编辑距离匹配已有 key |
| 2026-08-05 | 用户画像 value 合并：新增 mergeProfileValue()，tier 1 取更长，tier 2/3 取新值 |
| 2026-08-05 | 数据库 schema v3 升级：user_profile 表新增 last_decay_at 字段，与 last_triggered 职责分离 |
| 2026-08-05 | 衰减逻辑修复：基于 last_decay_at 衰减，恢复 WHERE ≥1小时 条件，避免重复扣分 |
| 2026-08-05 | tier 保留逻辑修复：qMin(tier, existingTier) 保留更稳定 tier |
| 2026-08-05 | 置信度作用收窄：仅用于遗忘机制+激活阈值，不再做 LLM 可靠性信号 |
| 2026-08-05 | LLM 注入格式变更：tier 标签（长期认知/近期观察/今日状态）替代百分比 |
| 2026-08-05 | AI 摘要传入现有画像：extractMemoryAsync 新增 existingProfiles 参数，Prompt 增加增量更新规则 |
| 2026-08-05 | TTS 文本切分修复：cut0（不切）→ cut5（按全部标点切），修正注释 |
| 2026-08-05 | 超分采样率适配：ApiTTSProvider 新增 m_sampleRate + getSampleRate()，流式模式 qobject_cast 获取实际采样率 |
| 2026-08-05 | 更新文档：ARCHITECTURE.md、development_log.md、project_tech_snapshot.md、ROADMAP.md、API_DOC.md、MODULE_CALL_DOC.md |
| 2026-08-02 | 播放器工厂模式重构：IPcmPlayer 静态工厂方法、TTSService 解耦具体播放器、FilePlayer::playFile→setSource |
| 2026-08-02 | 流式TTS播放架构修复：流式PCM解析、IPcmPlayer抽象、播放完成检测、预填充防吞开头 |
| 2026-08-02 | 修复 signals 遮蔽 bug：StreamPlayer 子类重复声明 PcmPlayerFinished 导致槽函数收不到 |
| 2026-08-02 | 修复野指针崩溃：m_buffer 未初始化 nullptr，析构时访问野指针 |
| 2026-08-02 | 修复 QNetworkReply 内存泄漏：finished lambda 中未释放 reply |
| 2026-08-02 | FilePlayer 修复：pushData 写入位置 bug + WAV头解析完善（通道数+位深） |
| 2026-08-02 | 更新文档：ARCHITECTURE.md、development_log.md、project_tech_snapshot.md、ROADMAP.md |
| 2026-07-29 | 实现TagValidator类，支持5级标签校验流程（精确匹配→编辑距离→继承→默认值） |
| 2026-07-29 | 更新文档：ARCHITECTURE.md、development_log.md、project_tech_snapshot.md、ROADMAP.md |
| 2026-07-28 | 数据库结构简化：删除parsed_json列，raw_reply直接存储JSON对象 |
| 2026-07-28 | LLM输出格式从标签协议迁移到JSON协议，添加response_format参数 |
| 2026-07-28 | 重构parseJsonReply()，从正则拆分改为JSON对象解析 |
| 2026-07-28 | 历史记忆注入优化：只注入纯文本内容，剥离标签格式 |
| 2026-07-26 | TTSService重构为策略模式，新增ITTSProvider接口和ApiTTSProvider实现 |
| 2026-07-26 | 接入GPT-SoVITS HTTP API，实现实时语音合成 |
| 2026-07-25 | TTSService双队列模型优化，集成Qt Multimedia实现音频播放 |
| 2026-07-23 | 新增用户画像管理（置信度衰减）和长期记忆摘要功能 |
| 2026-07-23 | MemoryManager新增getUnsummarizedTurns()，LLMService新增extractMemoryAsync() |
| 2026-07-23 | AppController集成记忆提取流程：检查未摘要对话数，触发后台提取 |
| 2026-07-22 | SettingsWidget新增记忆管理页，支持历史记录分页浏览、单条删除、清空全部 |
| 2026-07-22 | MemoryManager新增分页查询、计数、删除、清空方法 |
| 2026-07-22 | 修复ConfigManager配置文件损坏问题，修复SettingsWidget空指针访问问题 |
| 2026-07-19 | 新增 MemoryManager（AI记忆系统），使用SQLite数据库存储对话历史 |
| 2026-07-19 | LLMService::askDeepSeek 新增 historyQA 参数，支持传入对话历史上下文 |
| 2026-07-19 | AppController 集成 MemoryManager，实现对话历史的保存与加载（默认15轮短期记忆） |
| 2026-07-17 | ConfigManager实现配置文件持久化，LLMService实现提示词外部化和AI状态同步机制 |
| 2026-07-16 | 新增 AnchorManager、AnchorStrategy、ChatWidget，修复多个位置跟随问题 |
| 2026-07-15 | 新增句子解析、TTS服务、外观管理、AppController中枢 |
