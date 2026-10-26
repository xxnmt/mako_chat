# 开发记录

> 项目：Windows_AI 桌面看板娘（桌宠）
> 最后更新：2026-10-26

---

## 提交历史

| 哈希 | 日期 | 类型 | 描述 | 关键变更 |
|------|------|------|------|----------|
| `1646ff4` | 2026-10-26 | docs | 更新 HUMANIZATION_PLAN 协议端选型与 Phase 规划 | SnowLuma 选型、Phase 0.5 收缩裁决、记忆 v2 设计 |
| `b126180` | 2026-10-20 | docs | 补充 Phase 0 测试报告 | 人工异常回复校验、BUG-1/BUG-2 修复记录 |
| `485b5e3` | 2026-10-20 | fix | 修正 Levenshtein 空串边界 | 空标签不再误判为枚举首项（BUG-2） |
| `61f1767` | 2026-10-19 | feat | 实现大脑与渠道架构解耦 | Utterance/IChannelAdapter/SessionRouter、DB v2、identity.json、23 例测试 |
| `5e6facd` | 2026-10-19 | feat | 异常回复拦截与校验 | zh_text/ja_text/tags 缺失整轮抛弃，新增 replyAbnormal |
| `7dd8ce4` | 2026-07-16 | feat | 添加句子解析、TTS服务和外观管理功能 | 新增 AppController/TTSService/AppearanceManager/SentenceText，LLM多标签解析升级 |
| `88df6b2` | 2026-07-15 | refactor | 重构项目资源与应用架构 | 统一图片命名，BubbleWidget解耦attachTo，CharacterWidget瘦身 |
| `08a4808` | 2026-07-15 | docs | 添加GPT-SoVITS API接口文档 | 新增TTS接口文档 |
| `e296241` | 2026-07-14 | docs | 添加开发日志、路线图、架构设计文档与技术快照 | docs目录建立 |
| `c9f1f5f` | 2026-07-14 | refactor | 重构AI交互逻辑，抽离LLM服务模块 | 新增 LLMService 类，CharacterWidget通过信号槽通信 |
| `4e3126e` | 2026-07-14 | refactor | 添加立绘有效区域计算与气泡位置修正 | 新增 calculateVisibleRect() |
| `57b3251` | 2026-07-14 | build | 新增.gitignore文件 | 添加.gitignore |
| `cd2ae4e` | 2026-07-14 | chore | 清理构建目录临时文件 | 清理git索引 |
| `d654a9c` | 2026-07-13 | refactor | 重构主窗口部件并添加对话气泡组件 | 新增 BubbleWidget |
| `8381be2` | 2026-07-13 | feat | 添加浮动角色部件与DeepSeek AI聊天支持 | 可拖拽窗口+AI对话 |
| `0a3c5b7` | 2026-07-13 | feat | 集成DeepSeek AI网络请求功能 | QNetworkAccessManager |
| `d93db33` | 2026-07-13 | init | 简单demo窗口 | 项目初始化 |

---

## 里程碑

### M1: 项目初始化
- 日期：2026-07-13
- 提交：`d93db33`
- 内容：创建基础窗口框架，配置CMake构建环境

### M2: AI集成
- 日期：2026-07-13
- 提交：`0a3c5b7` → `8381be2`
- 内容：集成DeepSeek API，实现AI聊天功能，浮动角色窗口

### M3: UI组件化
- 日期：2026-07-13
- 提交：`d654a9c`
- 内容：添加BubbleWidget气泡组件，实现打字机特效

### M4: 交互优化
- 日期：2026-07-14
- 提交：`4e3126e` → `c9f1f5f`
- 内容：立绘有效区域计算，抽离LLMService模块

### M5: 架构重构
- 日期：2026-07-15
- 提交：`88df6b2`
- 内容：统一图片命名规范，BubbleWidget解耦（attachTo+eventFilter），CharacterWidget瘦身

### M6: 多标签协议 & TTS框架
- 日期：2026-07-16
- 提交：`7dd8ce4`
- 内容：
  - 新增 **AppController**（应用中枢，信号调度）
  - 新增 **AppearanceManager**（四维状态管理，立绘切换）
  - 新增 **TTSService**（生产者-消费者队列，模拟TTS）
  - 新增 **SentenceText** 数据结构
  - LLMService升级：多标签协议解析，多句拆分
  - 系统指令定义完整协议规范

### M7: 位置锚点系统 & Bug修复
- 日期：2026-07-16
- 内容：
  - 新增 **AnchorManager**（位置锚点管理，统一位置跟随）
  - 新增 **AnchorStrategy**（锚点位置枚举与配置结构）
  - 新增 **ChatWidget**（聊天输入窗口）
  - 修复气泡首次显示位置未初始化问题（bubbleShown信号）
  - 修复立绘切换时位置未更新问题（characterPathChanged信号连接）
  - 移除 BubbleWidget/ChatWidget 中重复的位置跟随逻辑
  - 实现 HeadRight 锚点策略
  - 修复 LLMService 构造函数中 API Key 未赋值问题（m_apiKey = apiKey）
  - 修复 IC-001 服务定位器反模式（LLMService改为构造函数注入apiKey）

### M8: 配置文件持久化
- 日期：2026-07-17
- 内容：
  - ConfigManager实现配置文件读写（JSON格式）
  - 配置文件路径：`app_data/config/setting.json`
  - 支持API Key和GPT-SoVITS服务地址的持久化
  - 首次启动自动创建配置文件，使用默认值

### M9: AI状态同步机制 & 提示词外部化（已实现）
- 日期：2026-07-17
- 内容：
  - LLMService实现状态提供者模式：`registerStateProvider()` 回调接口，`askDeepSeek()` 中动态追加状态上下文
  - LLMService实现提示词外部化：`initializePromptFile()` 首次启动自动释放默认prompt.txt，`loadSystemPrompt()` 从文件加载提示词
  - AppearanceManager新增状态描述方法：`getCurrentStateDescription()`、`getDistance()`、`getClothing()`、`getBlush()`、`getEmotion()`
  - AppController连接状态提供者：`m_llmService->registerStateProvider([this](){return m_appearance->getCurrentStateDescription();})`
  - 资源文件新增默认提示词路径：`image/default_config/prompt.txt`
  - 修复DeepSeek API请求格式错误：系统提示词包装为对象而非字符串

### M10: Git分支管理配置（已实现）
- 日期：2026-07-18
- 内容：
  - 创建 `docs` 分支（代码+文档，本地开发）
  - `main` 分支移除docs跟踪（仅代码，用于推送GitHub）
  - 更新 `.gitignore` 和 `.gitattributes` 配置
  - 清理已修复的技术债务文档标记

### M11: 设置界面实现 & 代码清理（已实现）
- 日期：2026-07-19
- 内容：
  - 新增 SettingsWidget 设置界面（API Key配置页）
  - 右键菜单"设置"入口已连接到SettingsWidget
  - 移除 CharacterWidget 中无用的 userChat 信号
  - 移除 AppController 中注释的自动问候代码
  - 简化Git分支管理（docs目录重新加入追踪）

### M12: AI记忆系统（已实现）
- 日期：2026-07-19
- 内容：
  - 新增 **MemoryManager**（AI记忆系统），使用SQLite数据库存储对话历史
  - 新增 **HistoryTurn** 数据结构，定义对话历史模型
  - LLMService::askDeepSeek 新增 historyQA 参数，支持传入对话历史上下文
  - sentenceReady 信号新增 rawReply 参数，用于保存原始回复
  - AppController 集成 MemoryManager，实现对话历史的保存与加载（默认15轮短期记忆）
  - 数据库路径：`app_data/memory/QianDaoMoZi_memory.db`
  - 数据库结构：chat_history表（id, timestamp, user_input, raw_reply, parsed_json）

### M13: 设置界面完善（记忆管理页）
- 日期：2026-07-22
- 内容：
  - SettingsWidget新增记忆管理页，支持历史记录查看、分页浏览、单条删除、清空全部
  - 历史记录删除需要API Key验证，防止误删
  - HistoryTurn数据结构新增 id 和 timestamp 字段
  - MemoryManager新增分页查询方法 getHistoryTurn(offset, limit)、计数方法 getTotalHistoryCount()、删除方法 deleteTurnByID() 和 clearAllHistory()
  - AppController中添加 setMemoryManager() 和 setMemoryLength() 调用，确保SettingsWidget正确获取MemoryManager实例

### M14: 代码修复与文档更新
- 日期：2026-07-22~2026-07-23
- 内容：
  - 修复ConfigManager配置文件损坏问题：配置缺失时使用默认值而非返回false
  - 修复SettingsWidget空指针访问问题：四个方法添加空指针检查后的return语句
  - 更新README.md：添加安装说明、使用指南、API参考、已知限制、故障排除章节
  - 更新ARCHITECTURE.md：补充MemoryManager、SettingsWidget完整描述，更新模块关系图

### M15: 架构审查与改进建议
- 日期：2026-07-23
- 内容：
  - **职责越界问题**：识别出4个职责越界问题（AppController、SettingsWidget、LLMService、MemoryManager）
  - **过分耦合问题**：识别出6个耦合问题（直接依赖、双向依赖、隐式耦合、头文件依赖、初始化顺序、循环包含）
  - **代码质量问题**：识别出6个代码质量问题（内存泄漏、魔法数字、硬编码路径、重复日志、未使用代码、注释不完整）
  - 更新ARCHITECTURE.md：新增第9章「架构问题分析」和第10章「改进建议」，包含短期、中期、长期改进计划

### M16: 用户画像与长期记忆系统（已实现）
- 日期：2026-07-23
- 内容：
  - MemoryManager新增用户画像管理：`upsertUserProfile()`、`getActiveUserProfiles()`、`deleteUserProfile()`
  - MemoryManager新增长期记忆摘要管理：`addLongTermSummary()`、`getLatestSummaries()`、`deleteLongTermSummary()`
  - MemoryManager新增置信度衰减机制：`scanAndApplyProfileDecay()`（三级半衰期：Tier1=-0.8/天，Tier2=-5.0/天，Tier3=-25.0/天）
  - MemoryManager新增未摘要对话查询：`getUnsummarizedTurns()`
  - LLMService新增记忆提取功能：`extractMemoryAsync()`（异步调用LLM分析对话，提取用户画像和长期摘要）
  - LLMService新增MemoryManager依赖：`setMemoryManager()`
  - AppController集成记忆提取流程：`handleMakoReply()`中判断未摘要对话数，触发后台记忆提取；`memoryExtractionReady`信号处理中写入用户画像和长期摘要
  - LLMService::askDeepSeek()新增用户画像和长期摘要注入：将活跃画像和最新摘要追加到系统提示词
  - 数据库v2升级：新增user_profile表（含索引）和long_term_summary表（含索引）
  - AppController启动时调用`scanAndApplyProfileDecay()`进行画像衰减扫描

### M17: TTS语音系统重构与GPT-SoVITS集成（已完成）
- 日期：2026-07-25~2026-07-26
- 内容：
  - **TTSService重构为策略模式**：新增 `ITTSProvider` 抽象接口，支持多种 TTS 实现
  - **新增 ApiTTSProvider**：通过 `QNetworkAccessManager` 调用 GPT-SoVITS HTTP API（POST /tts）
  - **新增 MockTTSProvider**：模拟实现，用于开发测试
  - **双队列模型优化**：`m_ttsQueue`（待合成）→ `m_playQueue`（待播放），配合状态锁实现合成与播放并发
  - **集成 Qt Multimedia**：`QMediaPlayer` + `QAudioOutput` 实现音频播放
  - **TTS 模式配置**：支持 `api` 和 `mock` 两种模式切换（`reloadProvider()` 动态加载）
  - **参考音频映射**：按情绪状态映射参考音频文件（7种情绪各对应一个 wav 文件）
  - **临时文件管理**：音频即用即删，不做缓存；播放完成后自动清理
  - **错误处理**：合成失败时跳过当前句，不阻塞后续队列
  - **TD-005 技术债务已清理**：TTS 框架、API 接入、双队列、播放全部完成

### M18: JSON输出协议迁移（已完成）
- 日期：2026-07-28
- 内容：
  - **LLM输出格式从标签协议迁移到JSON**：旧格式 `[emotion:xxx][blush:yyy]` → 新格式 `{"sentences": [{"zh_text": "...", "ja_text": "...", "tags": {...}}]}`
  - **添加 response_format 参数**：API 请求中添加 `response_format: {type: "json_object"}` 强制 LLM 输出合法 JSON
  - **更新系统提示词**：要求 LLM 输出指定的 JSON 格式，包含 zh_text、ja_text 和 tags 字段
  - **重构 parseJsonReply()**：从正则拆分改为 JSON 对象解析，提取句子和标签
  - **设计决策**：JSON 协议更稳定、更易扩展，避免正则匹配的复杂性和脆弱性

### M19: 数据库优化与标签校验（已完成）
- 日期：2026-07-28~2026-07-29
- 内容：
  - **数据库结构简化**：删除 `chat_history` 表的 `parsed_json` 列，`raw_reply` 直接存储完整 JSON 对象
  - **新增 TagValidator 类**：实现标签合法性校验，支持5级校验流程：
    1. 精确匹配 → 直接通过
    2. 大小写忽略匹配 → 修正大小写
    3. 编辑距离匹配 → 修正拼写错误（如 "closest" → "closer"）
    4. 继承上一状态 → 使用上一句的标签值
    5. 使用默认值 → 使用该标签的默认值
  - **实现 Levenshtein 编辑距离算法**：计算字符串编辑距离，阈值根据标签长度动态调整
  - **历史记忆注入优化**：只注入纯文本内容，不注入标签格式，避免影响 LLM 输出格式
  - **修复正则表达式**：标签剥离正则从 `\$$[^\$$]+\]` 修正为 `\\[[^\\]]+\\]`
  - **添加记忆提取结果处理**：`handleMakoReply()` 中检查未摘要对话数，触发后台记忆提取

### M20: 流式TTS播放架构与修复（已完成）
- 日期：2026-08-02
- 内容：
  - **流式PCM解析修复**：`ApiTTSProvider::readyRead` 识别服务端流式格式（首块44字节空WAV头 + 后续裸PCM），跳过WAV头后透传裸PCM；原代码遇到裸PCM就 `buffer.clear()` 导致数据全部丢失
  - **新增 IPcmPlayer 抽象接口**：统一 `StreamPlayer`（流式）和 `FilePlayer`（文件型）两种播放器的生命周期管理，定义 `startPlayer/stopPlayer/writePcm/setSynthesisDone` 虚方法 + `PcmPlayerFinished/PcmPlayerError` 信号
  - **StreamPlayer 播放完成检测机制**：
    - `setSynthesisDone()`：TTSService 在 onTtsFinished 中调用，标记所有 PCM 已到达
    - `onStateChanged(IdleState)`：QAudioSink 状态变 Idle 时检查 `queueEmpty && bufferDone && synthesisDone`
    - `checkPlayEnd()` 兜底定时器：500ms 间隔，连续3次（1.5秒）满足完成条件则触发
    - `finishAndEmit()`：统一停止定时器+emit PcmPlayerFinished
  - **修复 signals 遮蔽 bug**：StreamPlayer 子类重复声明 `PcmPlayerFinished` 信号导致 moc 生成两个独立信号，`emit` 发子类信号但 `connect` 连基类信号，槽函数永远收不到。删除子类重复 signals 声明
  - **修复野指针崩溃**：`m_buffer` 未初始化为 nullptr，StreamPlayer 未 startPlayer 就被 deleteLater 析构时，`stopPlayer` 中 `if(m_buffer)` 判定野指针为 true 访问崩溃
  - **修复 QNetworkReply 内存泄漏**：`finished` lambda 中 `delete ctx` 前未释放 `ctx->reply`
  - **预填充防吞开头机制**：
    - StreamPlayer：startPlayer 时从 m_queue 取出所有已到达 PCM，写入 m_buffer，seek(0) 后再 `QAudioSink::start`，避免设备冷启动期间数据丢失
    - FilePlayer：playFile 中 seek(44) 跳过 WAV 头后，预读取 CHUNK_SIZE*4 数据写入 m_buffer，再 resume()
  - **FilePlayer 修复**：
    - `pushData` 写入位置 bug：`m_buffer->write(data)` 在当前 pos 写入会覆盖正在播放的位置，改为保存读取位置→seek到末尾→write→恢复读取位置
    - WAV头解析完善：从偏移22读取通道数、偏移34读取位深，三个参数任一不同时重建 QAudioSink（原硬编码1通道/16位）
  - **修复 startPlayer 重置 m_isSynthesisDone 时序 bug**：startPlayer 中 `m_isSynthesisDone=false` 会清掉 setSynthesisDone 已设置的 true，导致 IdleState 检查永远失败
  - **技术债务清理**：TD-018~TD-022 全部修复

### M21: 播放器工厂模式重构（已完成）
- 日期：2026-08-02
- 内容：
  - **IPcmPlayer 静态工厂方法**：在 IPcmPlayer 接口中增加 `enum Type { Stream, File }` 和 `static IPcmPlayer* create(Type, QObject*)` 静态工厂方法，集中创建逻辑
  - **新增 ipcmplayer.cpp**：工厂方法实现文件，include streamplayer.h 和 fileplayer.h，头文件不依赖具体子类（无循环依赖）
  - **FilePlayer::playFile → setSource**：将 FilePlayer 特有的 `playFile` 方法改为 `setSource` 并 override IPcmPlayer 的虚方法，统一接口调用
  - **TTSService 解耦**：移除 ttsservice.cpp 对 streamplayer.h 和 fileplayer.h 的直接 include，改为只依赖 ipcmplayer.h；`new StreamPlayer` → `IPcmPlayer::create(Stream)`，`new FilePlayer` → `IPcmPlayer::create(File)`
  - **connect 信号统一**：文件播放分支从 `&FilePlayer::PcmPlayerFinished` 改为 `&IPcmPlayer::PcmPlayerFinished`（基类信号），与流式分支保持一致
  - **CMakeLists.txt**：新增 ipcmplayer.cpp 到构建列表
  - **技术债务清理**：TD-023 已修复

### M22: 用户画像系统改进 & TTS 修复（已完成）
- 日期：2026-08-05
- 内容：
  - **用户画像 key 归一化**：新增 normalizeProfileKey()，基于 Levenshtein 编辑距离匹配已有 key，解决同义 key 碎片化问题
  - **用户画像 value 合并**：新增 mergeProfileValue()，tier 1 取更长 value，tier 2/3 取新 value
  - **数据库 schema v3 升级**：user_profile 表新增 last_decay_at 字段，与 last_triggered 职责分离
  - **衰减逻辑修复**：基于 last_decay_at 衰减，恢复 WHERE ≥1小时 条件，避免重复扣分
  - **tier 保留逻辑修复**：qMin(tier, existingTier) 保留更稳定 tier
  - **置信度作用收窄**：仅用于遗忘机制+激活阈值，不再做 LLM 可靠性信号
  - **LLM 注入格式变更**：tier 标签（长期认知/近期观察/今日状态）替代百分比
  - **AI 摘要传入现有画像**：extractMemoryAsync 新增 existingProfiles 参数，Prompt 增加增量更新规则
  - **TTS 文本切分修复**：cut0（不切）→ cut5（按全部标点切），修正注释
  - **超分采样率适配**：ApiTTSProvider 新增 m_sampleRate + getSampleRate()，流式模式 qobject_cast 获取实际采样率
  - 技术债务清理：TD-024~TD-029 全部修复

### M23: 关系状态系统 & 记忆架构重整（已完成）
- 日期：2026-08-06~2026-08-07
- 内容：
  - **关系状态系统实现**：
    - 新增 `relationship_state` 表（v1 迁移块第 5 张表，dimension/value/updated_at，UNIQUE(dimension)），默认 intimacy=30/trust=30
    - MemoryManager 新增关系状态 CRUD：`upsertRelationshipState(dimension, delta)`（clamp 0-100）、`getRelationshipStates()`、`initRelationshipState()`
    - `RelationshipState` 结构体加入 historyturn.h（dimension/value/updatedAt）
  - **askDeepSeek 系统提示词注入**：新增 `# 我们的关系` 段落，渲染为 `亲密度: X/100`、`信任度: X/100`
  - **extractMemoryAsync 记忆提取扩展**：提取 `relationship_updates`（dimension + delta，范围 -5.0~+5.0），AppController `memoryExtractionReady` lambda 写入 relationship_state（delta≠0 才更新）
  - **记忆架构重整（废弃 user_profile 表）**：
    - 旧的 `user_profile` 表（含 confidence/tier/置信度衰减/normalizeProfileKey 旧签名/mergeProfileValue 旧签名）整体废弃
    - 用户/角色长期稳定特质改由 `character_profile` 表承载（简单 subject/key/value，无置信度无 tier，UNIQUE(subject,key)）
    - 事件类记忆改由 `episodic_memory` 表承载（content/event_time/importance/type/status/last_accessed/last_decay_at/source_ids，importance 衰减，decayEpisodicMemory 基于 last_decay_at）
    - `normalizeProfileKey`/`mergeProfileValue` 签名变更为带 subject 参数，服务于 character_profile 的 key 归一化
    - 数据库版本统一为 `PRAGMA user_version = 1`，单一 v1 迁移块创建 5 张表
  - **WorkingMemory 工作记忆模块移除**：曾实现内存级工作记忆（currentTopic/userMood/userIntent/contextSummary，零额外 API 成本，主对话 JSON 顺带输出），经评估为非必要，已彻底移除（llmservice.cpp/h、historyturn.h、prompt.txt 三处清理）
  - **prompt.txt 资源默认配置更新**：`image/default_config/prompt.txt` 从旧标签格式更新为 JSON 协议格式（sentences 数组 + 四维 tags）
  - **relationship_state 表创建位置修复**：原本在 initDatabase 中被无条件创建（v1 迁移块之外），现已移入 v1 迁移块作为第 5 张表，迁移归属正确
- 技术债务：无需新增条目（relationship_state 移位属代码整理非债务）

### M24: TimeManager 重构 & TTSProcessManager 孤儿进程清理（已完成）
- 日期：2026-08-08
- 内容：
  - **TimeManager 整文件重写**：
    - 删除：QElapsedTimer、m_isIdleTimerActive/m_isBlushTimerActive/m_isLLMActive 状态机、onMinuteTick 里的退火轮询
    - 新增：`QTimer *m_blushResetTimer` / `QTimer *m_emotionResetTimer`（均 singleShot）
    - 新增接口：`notifyBlushingApplicated()`
    - `notifyLLMEnded()` → 启动表情退火倒计时（m_BackIdleTime * 1000 ms）
    - `notifyBlushingApplicated()` → 启动脸红退火倒计时（m_blushTime * 1000 ms）
    - `notifyUserInputStarted()` → stop 两个定时器
    - `notifyLLMtagsApplicated()` → stop 表情+脸红两个定时器（修复退火时序错乱：原只停表情定时器，脸红退火可能在对话中途任意时刻触发；后改为两定时器都停，脸红由 `notifyBlushingApplicated()` 视情况重启）
    - `onMinuteTick()` 只保留服装时段切换（白天校服 / 夜晚睡衣）
    - 修复野指针崩溃（原 m_blushTimer/m_idleTimer 指针未初始化，TD-030）
    - 修复退火时间单位错误（原 hasExpired(15) 是 15ms，现 start(15000) 是 15秒，TD-032）
  - **TTSProcessManager 孤儿进程清理**：
    - 新增 `killProcessOnPort(int port)` 方法（Windows: netstat+taskkill）
    - `apiStart()` 开头加 TCP 探测端口占用 → 杀孤儿进程（TD-031）
    - 头部加 include: QRegularExpression、QThread（Q_OS_WIN）
    - 修复 "SoVITS 切换 400 Bad Request"（端口被占用导致复用坏实例）
    - 修复 "崩溃后留孤儿 python.exe"（异常退出后端口被占用）
  - **AppController 脸红退火触发**（appcontroller.cpp L146-149）：
    - `onPlayAudioAction` 中 `notifyLLMtagsApplicated()` 之后加判断：
      `if (tags.value("blush") == "blushing") m_timeManager->notifyBlushingApplicated();`
  - **TD 项核对**：
    - TD-014 AnchorManager 内存泄漏：✅ 非问题（仅持弱引用，widget 所有权归 AppController）
    - TD-015 LLMService 头文件依赖：✅ 已修复（llmservice.h L11 已是 `class MemoryManager;` 前向声明）
    - TD-013 AppController 上帝对象：维持 ⚠️ 可接受（TimeManager 已独立为子类）
    - TD-024/025：状态改为 ✅ 已废弃（user_profile 表整体删除）
    - TD-028/029：描述更新（normalizeProfileKey 带 subject / extractMemoryAsync 传 existingProfiles + existingMemories）
  - **注**：M22 段落描述的 user_profile 系统（置信度衰减、tier、last_decay_at）已在 M23 记忆架构重整中整体废弃，TD-024/025 状态同步标记为「已废弃」
  - 技术债务清理：TD-030~TD-032 全部修复，TD-014/015 核查结案

### M25: TTS 流式播放流程确认 & 采样率从WAV头读取（已完成）
- 日期：2026-08-11
- 内容：
  - **流式播放流程确认**：v3 模型"流式模式"实为**句子级分段返回**（return_fragment），非 token 级真流式；服务端按 cut5 切句逐句合成并 yield，播放器随到随放但粒度是句子（句1 播放与句2 合成并行）
  - **UI 同步时机**：emit playAudioAction 移至 processTtsQueue 首块 lambda（首块 PCM 到达即变更 UI），替代原 startStreamPlayer 中合成前触发
  - **采样率从 WAV 头读取（TD-033）**：
    - 删除 ApiTTSProvider 中按 `super_sampling` 猜测采样率的逻辑（原 `super_sampling ? 48000 : 24000`）
    - 流式 readyRead 跳过头时从 WAV 头偏移 24 读真实采样率写入 `m_sampleRate`；非流式分段响应从首个 WAV 头读取
    - TTSService 新增私有 helper `apiSampleRate()`，替换三处硬编码 `startPlayer(24000,...)`
    - IPcmPlayer 接口新增 `updateSampleRate()`（默认空实现），StreamPlayer 重写并在首块到达时校正 QAudioSink（与默认相同则空操作）
    - 采样率以服务端 WAV 头为准：v3=24000Hz / v1,v2=32000Hz / v4及超分=48000Hz
  - 技术债务清理：TD-033 修复

---

### M26: 退火机制重构——按「每轮最终状态」驱动（已完成）
- 日期：2026-08-11
- 内容：
  - **动机**：原退火逻辑按句子驱动、问题多——表情定时器轮末无条件启动（即使最终表情是默认 happyIdle 也白跑）、脸红逐句重启不反映轮末状态、无距离退火（closer 挂住不回落）
  - **新模型**：每轮对话结束（playbackQueueEmpty）取最终视觉状态，只对激活维度开各自退火定时器
    - `blush == "blushing"` → 脸红定时器（m_blushTime*1000，回落到 unblushing）
    - `emotion != "happyIdle"` → 表情定时器（m_BackIdleTime*1000，回落到 happyIdle）
    - `distance == "closer"` → 距离定时器（**新增** m_distanceTime*1000，回落到 far）
  - **TimeManager 重构**：删除 `notifyLLMEnded` / `notifyLLMtagsApplicated` / `notifyBlushingApplicated` 三方法，统一为 `notifyRoundEnded(finalTags)`（先停全部退火，再按最终状态启动）；新增 `m_distanceResetTimer` + `distanceReset()` 信号 + `setDistanceTime()`（默认 15s，与 setBlushTime/setBackIdleTime 同格式）
  - **AppController 调整**：`onPlayAudioAction` 删掉逐句定时器调用（只留 applyTags+showMessage）；`playbackQueueEmpty` 连接改为 lambda，从 AppearanceManager 读最终 getBlush/getEmotion/getDistance 组装 finalTags 调 `notifyRoundEnded`；新增 `distanceReset → distance="far"` 连接
  - 三个退火 setter（setBlushTime/setBackIdleTime/setDistanceTime）暂未接 config（下一阶段完善 config/setting 功能时接线）

---

### M29: Phase 0 大脑/渠道架构解耦（已完成）

- 日期：2026-10-19（实施）、2026-10-20（测试与缺陷修复）
- 提交：`61f1767`（代码）、`485b5e3`（BUG-2 修复）、`b126180`（测试报告）
- 内容：
  - 新增 8 个文件：`utterance.h`、`ichanneladapter.h`、`petchanneladapter.h/cpp`、`qqchanneladapter.h/cpp`、`sessionrouter.h/cpp`
  - 输入链路改为 `textSubmitted → SessionRouter::submitInput → requestReply → AppController → askDeepSeek`，`RequestMeta` 随轮次贯穿入库/摘要/关系更新
  - 输出链路分层：气泡等渠道私有表现归 `PetChannelAdapter`，四维立绘（全局情绪层）仍由 AppController 直接驱动
  - 数据库 v2 迁移：渠道双键、speaker/sensitive、episodic origin、relationship_state 按 subject 重建、raw_messages 建表（只建不写）
  - ConfigManager 新增 identity.json 懒加载解析（self/master/盲区/立绘跟随/主动对话），Phase 0 无调度行为
  - 测试基线：tst_memorymanager(10) + tst_configmanager(8) + tst_sessionrouter(5) = 23 例全绿
  - 修复 BUG-1（`getUnsummarizedTurns` 先 bind 后 `exec(sql)` 丢参数，导致长期记忆摘要链路失效）、BUG-2（Levenshtein 空串分支写反，缺失 clothing 标签被误改为 pajama）
  - 与方案偏差 3 处：relationship_state 走「复制重建」（SQLite 无法改 UNIQUE 约束）、配置用 identity.json 而非 QSettings（跟随项目 JSON 体系）、迁移用 `user_version` v1→v2（沿用现有惯例）

---

## 技术债务

| 编号 | 描述 | 状态 | 优先级 | 引入版本 |
|------|------|------|--------|----------|
| TD-001 | API Key硬编码在ConfigManager构造函数中 | ✅ 已修复（配置文件持久化） | 高 | v0.1.0 |
| TD-002 | 立绘路径硬编码（已部分解决，AppearanceManager统一生成） | ✅ 已解决 | 高 | v0.1.0 |
| TD-003 | BubbleWidget位置计算硬编码（已解决，attachTo解耦） | ✅ 已解决 | 中 | v0.1.0 |
| TD-004 | 立绘切换功能未实现（已实现，AppearanceManager） | ✅ 已解决 | 高 | v0.1.0 |
| TD-005 | TTS为模拟实现，未接入真实引擎 | ✅ 已修复（策略模式+GPT-SoVITS API接入完成） | 高 | v0.2.0 |
| TD-006 | 用户输入入口缺失（硬编码测试文本） | ✅ 已修复（ChatWidget实现，移除userChat死信号） | 高 | v0.2.0 |
| TD-007 | characterwidget.cpp大量注释代码残留 | ✅ 已清理 | 中 | v0.2.0 |
| TD-008 | appcontroller.cpp重复include头文件 | ✅ 已修复 | 低 | v0.2.0 |
| TD-009 | 系统提示词硬编码在LLMService源码中 | ✅ 已修复（提示词外部化，prompt.txt文件） | 中 | v0.2.0 |
| TD-010 | AI记忆系统未实现 | ✅ 已实现（MemoryManager，SQLite短期记忆） | 高 | v0.2.4 |
| TD-011 | 对话历史查看界面缺失 | ✅ 已实现（SettingsWidget记忆管理页） | 中 | v0.4.0 |
| TD-012 | 长期记忆（重要事件摘要）未实现 | ✅ 已实现（LLM自动提取摘要+用户画像） | 中 | v0.4.0 |
| TD-013 | AppController职责过重（上帝对象） | ⚠️ 可接受（当前规模合适，TimeManager 已独立为子类） | 高 | v0.2.0 |
| TD-014 | AnchorManager内存泄漏风险 | ✅ 非问题（仅持弱引用，widget 所有权归 AppController） | 高 | v0.2.1 |
| TD-015 | LLMService头文件依赖MemoryManager | ✅ 已修复（llmservice.h 已用 `class MemoryManager;` 前向声明） | 中 | v0.2.4 |
| TD-016 | AppController中魔法数字SUMMARY_THRESHOLD | ✅ 非问题（设计意图：摘要轮数=短期记忆轮数） | 低 | v0.2.4 |
| TD-017 | LLM标签输出格式不稳定 | ✅ 已修复（JSON协议+TagValidator校验） | 高 | v0.5.0 |
| TD-018 | 流式PCM解析丢失数据 | ✅ 已修复（readyRead跳过WAV头透传裸PCM） | 高 | v0.5.0 |
| TD-019 | StreamPlayer野指针崩溃 | ✅ 已修复（m_buffer初始化nullptr+signals去重） | 高 | v0.6.0 |
| TD-020 | 播放完成信号不触发 | ✅ 已修复（setSynthesisDone+IdleState+兜底定时器） | 高 | v0.6.0 |
| TD-021 | 播放吞开头 | ✅ 已修复（startPlayer预填充PCM再启动QAudioSink） | 中 | v0.6.0 |
| TD-022 | FilePlayer写入位置错误 | ✅ 已修复（pushData先seek到末尾再write） | 中 | v0.6.0 |
| TD-023 | TTSService直接new具体播放器 | ✅ 已修复（IPcmPlayer 静态工厂方法） | 低 | v0.6.0 |
| TD-024 | 衰减重复扣分（基于 last_triggered 衰减导致每次 upsert 都扣分） | ✅ 已废弃（user_profile 表整体删除，confidence/last_triggered 字段不再存在） | 高 | v0.6.0 |
| TD-025 | tier 保留逻辑反转（qMax 误用导致 tier 升级而非保留稳定 tier） | ✅ 已废弃（tier 字段随 user_profile 表删除） | 高 | v0.6.0 |
| TD-026 | TTS 文本切分错误（cut0 不切导致长文本一次性合成失败） | ✅ 已修复（cut0→cut5 按全部标点切） | 中 | v0.6.0 |
| TD-027 | 超分采样率不匹配（super_sampling=true 时输出 48000Hz 但播放器硬编码 24000Hz） | ✅ 已修复（m_sampleRate 动态适配） | 中 | v0.6.0 |
| TD-028 | 角色档案 key 碎片化（同义 key 未归并导致档案重复） | ✅ 已修复（normalizeProfileKey 带 subject 参数，服务于 character_profile） | 中 | v0.6.0 |
| TD-029 | AI 摘要盲提取（无现有档案上下文导致重复/矛盾提取） | ✅ 已修复（extractMemoryAsync 传 existingProfiles + existingMemories 增量更新） | 中 | v0.6.0 |
| TD-030 | TimeManager 野指针崩溃（QElapsedTimer/m_blushTimer/m_idleTimer 指针未初始化） | ✅ 已修复（重构为 QTimer singleShot，m_blushResetTimer/m_emotionResetTimer） | 高 | v0.6.0 |
| TD-031 | TTSProcessManager 孤儿进程（异常退出后端口被占用导致下次启动复用坏实例） | ✅ 已修复（apiStart 开头 killProcessOnPort 清理占端口孤儿 python.exe） | 高 | v0.6.0 |
| TD-032 | TimeManager 退火时间单位错误（hasExpired(15) 误为 15ms，配置为 15秒） | ✅ 已修复（改用 QTimer::start(15000) 正确为 15秒） | 高 | v0.6.0 |
| TD-033 | 采样率硬编码/猜测（super_sampling 猜测 + 播放器硬编码 24000） | ✅ 已修复（流式/分段路径从服务端WAV头读取真实采样率，StreamPlayer::updateSampleRate 校正） | 中 | v0.6.0 |
| TD-034 | 对话轮次冲突：playbackQueueEmpty 裸信号无轮次身份，上一轮退火在下一轮对话中途误触发 | ⚠️ 待处理（归属「对话轮次冲突」特性，未来提供打断当前轮/自动排队两种策略） | 高 | v0.6.0 |
| TD-035 | TimeManager 昼夜判断逻辑颠倒（`now <= 22:00 || now > 7:00` 将整个白天 7:00-22:00 误判为夜，导致白天穿睡衣） | ✅ 已修复（跨零点判断改为 `now >= 22:00 || now < 7:00`） | 高 | v0.6.0 |
| TD-036 | 立绘切换抖动/漂移（closer 相对 far 脚底差 30px，center 锚定让 closer 脚底向下坠，且 resize+move 两步重绘造成向右闪帧） | ✅ 已修复（脚底中心锚定 + 原子 setGeometry + repaint + WA_NoSystemBackground） | 高 | v0.6.0 |
| TD-037 | 设置界面三组控件未接线（长期记忆摘轮数/夜晚时段/退火时长），点击保存无反应 | ⚠️ 待处理（控件与按钮已就位，缺 ConfigManager 配置项与槽函数；TimeManager 默认值硬编码） | 中 | v0.6.0 |

---

## 里程碑补充

### M27: 立绘切换修复 & 昼夜判断修正（已完成）

- 日期：2026-08-23
- 内容：
  - **TD-036 立绘切换抖动/漂移修复**：`updatePath` 改为锚定「脚底中心」（两张立绘 bottom 均为 767，脚底对齐则角色原地生长，不再 center 锚定造成 closer 脚底下坠）；移动改用原子 `setGeometry` 并在移动前 `repaint()` 同步渲染新图，配合构造器 `WA_NoSystemBackground`，消除切换瞬间旧图向右闪帧
  - **TD-035 昼夜判断跨零点修正**：原 `isNight = (now <= m_nightStart || now > m_nightEnd)` 逻辑颠倒，把白天 7:00-22:00 全判为夜（白天穿睡衣）；改为 `isNight = (now >= m_nightStart || now < m_nightEnd)`，正确覆盖 22:00-次日 7:00
  - **TD-034 对话轮次冲突**：`playbackQueueEmpty` 裸信号无轮次身份，导致上一轮退火在下一轮中途误触发。暂缓处理，归属 v0.7.0「对话轮次冲突」特性（未来让用户选择打断当前轮 / 自动进入队列）

### M28: 设置界面重构 —— 滚动主从布局 & 双向同步（已完成）

- 日期：2026-08-25
- 内容：
  - **删除旧 SettingsWidget（多标签页实现）**：`settingswidget.h/cpp/ui` 移除
  - **新设置界面改为「左侧目录树 + 右侧滚动内容」主从布局**：
    - 左侧 `treeIndex` 目录，右侧 `scrollArea`/`contentWidget` 内容按区块竖排
    - 两级分区：一级 `section1_*`（对话系统/语音合成/记忆系统/时间管理/历史会话记录）+ 二级 `section2_*`（大语言模型/GPT-Sovits/短期记忆/长期记忆/昼夜切换/自动退火）
  - **双向 scroll-spy 同步**：点击目录项 → `ensureWidgetVisible` 滚到对应分区；滚动右侧 → 按分区页面位置（`mapTo(viewport).y()` 取最贴近视口顶部的分区）高亮左侧目录项。用 `m_sectionMap`（目录项↔分区映射）+ `m_syncGuard` 双向互斥防抖
  - **滚轮透传**：下拉框/微调框/时间编辑框安装 `eventFilter`，滚轮事件转给右侧滚动条并消费，不拦截界面滚动
  - **TTS模型切换接线**：GPT/SoVITS 模型保存成功后 `emit ttsModelSwitchRequested(gptPath, sovitsPath)`，AppController 连接 `m_ttsService->switchModel()` 实现热切换
  - **类/文件重命名**：`SettingWidgetTest` → `SettingsWidget`，`settingwidgettest.*` → `settingswidget.*`（git mv 保留历史），成员 `m_settingWidgetTest` → `m_settingsWidget`
  - **待接线**：长期记忆自动摘轮数、时间管理（夜晚时间/退火时长）控件已就位但 config 未接线（下一阶段补 config/setting 功能时接线）

---

## 架构演进记录

| 阶段 | 架构模式 | 核心模块 | 特点 |
|------|----------|----------|------|
| v0.1.0 | 单体式 | CharacterWidget（大而全） | LLM/Bubble/Image全部内嵌 |
| v0.1.5 | 初步解耦 | CharacterWidget + LLMService | 网络层抽离，信号槽通信 |
| v0.2.0 | 中央控制器 | AppController + 6个独立模块 | 中枢调度，模块独立，多标签协议 |
| v0.3.0 | AI资源管理 | AnchorManager + ConfigManager + LLMService + AppearanceManager | 位置锚点系统、配置持久化（API Key/TTS地址）、状态提供者模式（动态追加状态）、提示词外部化（prompt.txt）、DeepSeek API格式修复 |
| v0.4.0 | AI记忆系统 | MemoryManager + LLMService + SettingsWidget | SQLite数据库（chat_history/user_profile/long_term_summary三张表）、短期记忆查询（默认15轮）、用户画像（置信度衰减三级半衰期）、长期记忆摘要（LLM自动提取）、记忆提取异步流程、记忆管理界面（查看/删除/清空/分页）、配置文件损坏降级处理、代码健壮性提升 |
| v0.4.1 | TTS策略模式 | TTSService + ITTSProvider + ApiTTSProvider | 策略模式重构TTS、GPT-SoVITS HTTP API接入、双队列模型优化（合成→播放）、Qt Multimedia集成、参考音频情绪映射、错误处理（合成失败跳过不阻塞） |
| v0.5.0 | JSON协议与标签校验 | LLMService + TagValidator + MemoryManager | JSON输出协议替代标签格式、response_format参数强制JSON输出、TagValidator标签校验与编辑距离修正、数据库结构简化（删除parsed_json列）、历史记忆纯文本注入 |
| v0.6.0 | 流式TTS播放与播放器抽象 & 时间管理 | TTSService + IPcmPlayer + StreamPlayer + FilePlayer + TimeManager + TTSProcessManager | 流式PCM播放（streaming_mode+pcmDataReady）、IPcmPlayer抽象接口（静态工厂方法）、播放完成检测（setSynthesisDone+IdleState+兜底定时器）、预填充防吞开头、FilePlayer WAV头解析完善、signals遮蔽bug修复、野指针崩溃修复、TimeManager QTimer singleShot 退火机制、TTSProcessManager 孤儿进程清理（killProcessOnPort） |
| v0.7.0-alpha | 大脑/渠道解耦（Phase 0） | SessionRouter + IChannelAdapter + PetChannelAdapter + QQChannelAdapter(占位) + MemoryManager v2 + ConfigManager(identity.json) | 统一 Utterance/RequestMeta 协议、渠道适配器、输入路由、记忆按 channel+session 双键隔离、身份层配置预留、23 例自动化测试 |

---

## 修复记录

| 日期 | 问题描述 | 修复方式 | 提交 |
|------|----------|----------|------|
| 2026-07-14 | 气泡位置偏移，跟随立绘不准确 | 实现 calculateVisibleRect() 计算立绘有效区域 | `4e3126e` |
| 2026-07-14 | AI交互逻辑与UI强耦合 | 抽离 LLMService 模块，使用信号槽通信 | `c9f1f5f` |
| 2026-07-15 | 图片命名混乱，不利于程序控制 | 统一为7种emotion规范命名 | `88df6b2` |
| 2026-07-15 | BubbleWidget与CharacterWidget紧耦合 | 实现attachTo()+eventFilter解耦 | `88df6b2` |
| 2026-07-16 | 立绘切换逻辑缺失 | 新增AppearanceManager四维管理状态 | `7dd8ce4` |
| 2026-07-16 | 单句回复限制，无法多句对话 | LLMService多句拆分+SentenceText结构 | `7dd8ce4` |
| 2026-07-16 | 气泡首次显示位置未初始化 | 新增bubbleShown信号，连接到updateAllAnchors | v0.2.1 |
| 2026-07-16 | 立绘切换时位置未更新 | 连接characterPathChanged到updateAllAnchors | v0.2.1 |
| 2026-07-16 | 位置跟随逻辑重复 | 移除BubbleWidget/ChatWidget的attachTo/eventFilter/updatePosition | v0.2.1 |
| 2026-07-16 | HeadRight锚点未实现 | 在calculatePosition中补全HeadRight分支 | v0.2.1 |
| 2026-07-17 | API Key硬编码 | ConfigManager实现JSON配置文件读写，支持API Key和TTS服务地址持久化 | v0.2.2 |
| 2026-07-17 | AI状态同步机制缺失 | LLMService实现状态提供者模式，AppearanceManager新增状态描述方法，动态追加状态到提示词 | v0.2.3 |
| 2026-07-17 | 提示词外部化框架缺失 | LLMService实现initializePromptFile()和loadSystemPrompt()，首次启动自动释放默认提示词 | v0.2.3 |
| 2026-07-22 | SettingsWidget数据库获取失败 | AppController中添加setMemoryManager()调用，确保SettingsWidget正确获取MemoryManager实例 | v0.2.5 |
| 2026-07-22 | SettingsWidget空指针解引用 | 在refreshHistoryTurnList、loadHistoryPage、on_btn_deleteSelectedMemory_clicked、on_btn_claenAllMemory_clicked四个方法中添加空指针检查后的return语句 | v0.2.5 |
| 2026-07-22 | ConfigManager配置文件损坏 | 修改loadSetting()方法，配置缺失时使用默认值并继续执行，而非返回false | v0.2.5 |
| 2026-07-25 | TTS 400 Bad Request | 定位根因：参考音频文件名不匹配（conscientious_idle_01.wav → conscientious_01.wav），需修改配置中的文件名 | v0.4.1 |
| 2026-07-28 | LLM标签输出格式不稳定 | 迁移到JSON输出协议，添加response_format参数强制JSON输出 | v0.5.0 |
| 2026-07-28 | 数据库parsed_json字段冗余 | 简化数据库结构，raw_reply直接存储JSON对象 | v0.5.0 |
| 2026-07-28 | 标签校验缺失导致非法标签（如"closest"） | 实现TagValidator类，支持编辑距离修正 | v0.5.0 |
| 2026-07-28 | 历史记忆注入标签格式影响LLM输出 | 优化为纯文本注入，剥离标签格式 | v0.5.0 |
| 2026-07-29 | 正则表达式语法错误 | 修正标签剥离正则为 `\\[[^\\]]+\\]` | v0.5.0 |
| 2026-08-08 | TimeManager 野指针崩溃（m_blushTimer/m_idleTimer 指针未初始化） | 重构为 QTimer singleShot（m_blushResetTimer/m_emotionResetTimer），删除 QElapsedTimer 状态机 | v0.6.0 |
| 2026-08-08 | TimeManager 退火时间单位错误（hasExpired(15) 误为 15ms） | 改用 QTimer::start(15000) 正确为 15秒 | v0.6.0 |
| 2026-08-08 | SoVITS 切换 400 Bad Request（端口被孤儿 python.exe 占用） | TTSProcessManager::apiStart 开头 TCP 探测端口占用 → killProcessOnPort 清理孤儿进程 | v0.6.0 |
| 2026-08-08 | 崩溃后留孤儿 python.exe（异常退出端口未释放） | killProcessOnPort 在 apiStart 开头强制清理占端口进程 | v0.6.0 |
| 2026-10-19 | 未摘要历史查询恒为空（prepare+bind 后又 exec(sql)，参数被丢弃，`channel = NULL` 恒不成立） | 改为 `query.exec()` 复用预编译语句，恢复长期记忆摘要链路（BUG-1） | `61f1767` |
| 2026-10-20 | 空标签被编辑距离误判为枚举首项（`distance("", x)` 恒为 0，缺失 clothing 被改成 pajama） | 修正 Levenshtein 空串边界（`distance("", x) = len(x)`），空标签回到「继承 → 默认值」降级链（BUG-2） | `485b5e3` |

---

## 代码审查记录

| 日期 | 审查内容 | 发现问题 | 处理方式 |
|------|----------|----------|----------|
| 2026-07-14 | 仓库清理 | 大量构建产物被追踪 | 添加.gitignore，清理git索引 |
| 2026-07-15 | 资源命名 | 图片命名不统一 | 批量重命名为规范格式 | - |
| 2026-07-16 | 架构评审 | 模块职责清晰，信号槽连接合理 | 通过 |
| 2026-07-23 | 架构审查 | 识别16个技术债务项 | 更新ARCHITECTURE.md，添加改进建议 |
| 2026-07-29 | JSON协议审查 | LLM输出格式不一致，标签校验缺失 | 实现JSON协议+TagValidator，完成修复 |
| 2026-10-19 | Phase 0 完整性审查（A-E 共 14 项） | 未摘要游标与摘要分桶失配、群聊「只存参与」导致旁听失忆 | 修正方案：`long_term_summary` 双键、`raw_messages` 原始层+空闲蒸馏层 |
