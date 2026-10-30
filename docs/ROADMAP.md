# 开发路线图

> 项目：Windows_AI 桌面看板娘（桌宠）
> 版本：v0.7.0-rc6-dev（Phase 0/0.5、Phase 1 前置、QQ 文本、记忆系统 v5 R3 + PR-4 MemoryAgent 完成）
> 更新日期：2026-10-29

---

## 项目愿景

打造一款功能完善、交互丰富的桌面看板娘应用，支持AI对话、立绘切换、语音交互、智能助手等核心功能。

## 项目定位

相比同类项目（如 LingChat），本项目专注于：
- **科学的记忆系统**：五层记忆架构（短期记忆+角色档案+情景记忆+长期摘要+关系状态）+ 情景记忆重要度衰减机制 + 关系状态量化驱动
- **系统化的状态管理**：四维立绘状态（emotion/blush/distance/clothing）设计 + 标签合法性校验
- **稳定的AI协议**：JSON输出格式 + response_format 强制JSON + TagValidator 编辑距离修正
- **轻量级高性能**：C++/Qt 方案，内存占用和性能优于 Electron/Tauri 方案

---

## 版本规划

### v0.1.0 - 基础版本
**状态**：✅ 已完成

- [x] 浮动角色窗口（无边框、透明背景、置顶）
- [x] 鼠标拖拽移动
- [x] DeepSeek AI对话集成
- [x] 对话气泡显示
- [x] 打字机特效
- [x] 立绘有效区域计算

### v0.2.0 - 架构重构 & 多标签协议
**状态**：✅ 已完成

- [x] AppController中央控制器
- [x] AppearanceManager四维状态管理
- [x] TTSService生产者-消费者框架（模拟实现）
- [x] SentenceText数据结构
- [x] LLMService多标签协议解析
- [x] 多句拆分支持
- [x] 统一图片命名规范（7种emotion）
- [x] BubbleWidget解耦（attachTo + eventFilter）
- [x] 立绘切换系统（emotion/blush/distance/clothing）
- [x] 脸红自动退热机制

### v0.3.0 - AI资源管理
**状态**：✅ 已完成

**功能清单**：
- [x] 配置文件持久化（app_data/config/setting.json）
  - [x] API Key存储与读取
  - [x] GPT-SoVITS服务地址配置
  - [ ] TTS参数配置（语速、温度、参考音频）
- [x] 系统提示词外部化（app_data/config/prompt.txt）
  - [x] 框架预留：LLMService预留 `loadSystemPrompt()` 方法
  - [x] 提示词从文件加载
  - [x] 支持动态追加当前状态（方案C）
- [x] AI状态同步机制（方案C：本地切换后在下轮对话同步给AI）
  - [x] 框架预留：LLMService状态提供者模式
  - [x] 框架预留：AppearanceManager状态描述方法
  - [x] 框架预留：AppController连接状态提供者
  - [x] 实际实现：动态构建包含状态的系统提示词
- [x] 设置界面（SettingsWidget）
  - [x] API配置页
  - [x] 记忆配置页（记忆长度、历史记录管理）

### v0.4.0 - AI记忆系统
**状态**：✅ 已完成

**功能清单**：
- [x] 分层记忆系统
  - [x] 短期记忆（最近N轮对话上下文，SQLite）✅ 已实现（默认15轮）
  - [x] 角色档案（character_profile 表，subject/key/value，区分 user/mako 主体，无置信度无 tier）✅ 已实现
  - [x] 情景记忆（episodic_memory 表，事件/承诺/冲突/里程碑，importance 衰减）✅ 已实现
  - [x] 长期记忆摘要（LLM自动提取重要事件）✅ 已实现
  - [x] 关系状态（relationship_state 表，intimacy/trust 量化驱动）✅ 已实现
- [x] 对话历史查看界面 ✅ 已实现
  - [x] 历史记录列表（表格展示，分页浏览）
  - [x] 删除功能（单条删除、清空全部）
  - [ ] 导出功能
- [x] AI记忆写入与读取 ✅ 已实现
  - [x] 每次对话后写入历史
  - [x] 对话前读取短期记忆
  - [x] 角色档案和长期摘要注入LLM提示词
  - [x] 自动记忆提取（未摘要对话达到阈值时触发，传入已有档案做增量更新）

### v0.4.1 - TTS策略模式重构
**状态**：✅ 已完成

**功能清单**：
- [x] TTSService重构为策略模式（ITTSProvider抽象接口）
- [x] 新增ApiTTSProvider（GPT-SoVITS HTTP API接入）
- [x] 新增MockTTSProvider（模拟实现）
- [x] 双队列模型优化（合成队列→播放队列）
- [x] 集成Qt Multimedia音频播放
- [x] TTS模式配置（api/mock切换）
- [x] 错误处理（合成失败跳过，不阻塞队列）
- [x] 临时音频即用即删

### v0.5.0 - JSON协议 & 标签校验（当前版本）
**状态**：✅ 已完成

**功能清单**：
- [x] LLM输出格式从标签协议迁移到JSON协议
  - [x] 旧格式 `[emotion:xxx][blush:yyy]` → 新格式 `{"sentences": [...]}`
  - [x] 添加 `response_format: {type: "json_object"}` 强制JSON输出
  - [x] 更新系统提示词，要求JSON格式输出
- [x] 重构 `parseJsonReply()`：从正则拆分改为JSON对象解析
- [x] 新增TagValidator标签校验类
  - [x] 5级校验流程：精确匹配→大小写忽略→编辑距离→继承上一状态→使用默认值
  - [x] 实现Levenshtein编辑距离算法
  - [x] 编辑距离阈值根据标签长度动态调整
- [x] 数据库结构简化
  - [x] 删除 `chat_history` 表的 `parsed_json` 列
  - [x] `raw_reply` 直接存储完整JSON对象
- [x] 历史记忆注入优化
  - [x] 只注入纯文本内容，不注入标签格式
  - [x] 剥离 `[xxx:yyy]` 格式标签
- [x] 修复正则表达式语法错误

**技术债务清理**：
- [x] TD-017: LLM标签输出格式不稳定 → JSON协议+TagValidator校验
- [x] TD-017: 数据库冗余字段 → 删除parsed_json列

### v0.6.0 - 流式TTS播放 & 播放器抽象（当前版本）
**状态**：� 进行中

**功能清单**：
- [x] 流式PCM播放架构
  - [x] ApiTTSProvider 流式合成（streaming_mode=true）
  - [x] pcmDataReady 信号实时推送 PCM 数据
  - [x] readyRead 跳过首个WAV头后透传裸PCM
- [x] IPcmPlayer 播放器抽象接口
  - [x] StreamPlayer（流式PCM播放）
  - [x] FilePlayer（文件型WAV播放）
  - [x] 统一 startPlayer/stopPlayer/writePcm 接口
  - [x] 统一 PcmPlayerFinished/PcmPlayerError 信号
- [x] 播放完成检测机制
  - [x] setSynthesisDone() 标记合成完成
  - [x] onStateChanged(IdleState) 三条件检测
  - [x] checkPlayEnd() 兜底定时器（500ms × 3次）
- [x] 预填充防吞开头机制
  - [x] StreamPlayer：startPlayer 时预填充队列PCM
  - [x] FilePlayer：setSource 时预读取4×CHUNK_SIZE数据
- [x] FilePlayer 修复与完善
  - [x] pushData 写入位置 bug 修复
  - [x] WAV头解析完善（通道数+位深）
- [x] 播放器工厂模式重构（IPcmPlayer 静态工厂方法）
- [x] 角色档案系统改进（key 归一化 normalizeProfileKey、value 合并 mergeProfileValue，服务于 character_profile 表）
- [x] TTS 文本切分修复（cut5 按全部标点切，替代 cut0 不切）
- [x] 超分采样率适配（采样率从服务端WAV头读取，流式 StreamPlayer::updateSampleRate 校正 / 非流式 WAV 头解析）
- [x] AI 摘要传入现有画像（extractMemoryAsync 新增 existingProfiles + existingMemories 参数，增量更新替代盲提取）
- [x] 关系状态系统（relationship_state 表，intimacy/trust 默认30，askDeepSeek 注入 `# 我们的关系`，extractMemoryAsync 提取 relationship_updates）
- [x] 记忆架构重整（废弃 user_profile 表，改为 character_profile + episodic_memory + relationship_state 三张新表，数据库统一 v1 单一迁移块）
- [x] prompt.txt 资源默认配置更新为 JSON 协议格式
- [x] TimeManager 时间管理器（独立子类，QTimer singleShot 退火机制，onMinuteTick 服装时段切换）
- [x] TTSProcessManager 孤儿进程清理（apiStart 开头 TCP 探测端口占用 → killProcessOnPort 杀孤儿 python.exe，修复 SoVITS 切换 400 Bad Request）
- [ ] 设置界面完善
  - [x] 设置界面重构 ✅ 已实现（改为「左侧目录树 + 右侧滚动内容」主从布局，双向 scroll-spy 同步）
  - [x] 滚轮透传 ✅ 已实现（下拉框/微调框/时间编辑框不拦截界面滚动）
  - [x] TTS模型切换 ✅ 已实现（语音合成区：GPT路径/模型、SoVITS路径/模型，保存时热切换）
  - [x] 长期记忆自动摘轮数、时间管理（夜晚时间/退火时长）接线 ✅（TD-037，配置见 `memory.summary_length` 与 `time.*`）
  - [ ] 外观配置（气泡样式、窗口透明度）
  - [ ] TTS语速、温度配置
  - [ ] 快捷键配置（全局快捷键）
- [x] 时间驱动服装切换（白天校服 / 夜晚睡衣，TimeManager::onMinuteTick 实现）
- [ ] 右键菜单扩展（服装/表情快捷切换）
- [ ] 系统托盘图标
- [ ] 历史记录导出功能

**技术任务**：
- [x] 修复流式PCM解析丢失数据（TD-018）
- [x] 修复StreamPlayer野指针崩溃（TD-019）
- [x] 修复播放完成信号不触发（TD-020）
- [x] 修复播放吞开头（TD-021）
- [x] 修复FilePlayer写入位置错误（TD-022）
- [x] 衰减重复扣分已废弃（TD-024，user_profile 表整体删除，confidence/last_triggered 字段不再存在）
- [x] tier 保留逻辑反转已废弃（TD-025，tier 字段随 user_profile 表删除）
- [x] 修复 TTS 文本切分错误（TD-026，cut0→cut5）
- [x] 修复超分采样率不匹配（TD-027，m_sampleRate 动态适配）
- [x] 修复角色档案 key 碎片化（TD-028，normalizeProfileKey 带 subject 参数，服务于 character_profile）
- [x] 修复 AI 摘要盲提取（TD-029，extractMemoryAsync 传 existingProfiles + existingMemories 做增量更新）
- [x] 创建TimeManager时间管理器（独立子类，QTimer singleShot 退火机制）
- [x] 修复TimeManager野指针崩溃（TD-030，QElapsedTimer 指针未初始化 → 重构为 QTimer singleShot）
- [x] 修复TTSProcessManager孤儿进程（TD-031，apiStart 开头 killProcessOnPort 清理占端口孤儿 python.exe）
- [x] 修复TimeManager退火时间单位错误（TD-032，hasExpired(15) 误为 15ms → QTimer::start(15000) 正确为 15秒）
- [x] 采样率从WAV头读取（TD-033，流式/分段路径从服务端WAV头偏移24读真实采样率，替代 super_sampling 猜测与硬编码 24000）
- [x] AnchorManager内存泄漏核查（TD-014，非问题：仅持弱引用，widget 所有权归 AppController）
- [x] 优化LLMService头文件依赖（TD-015，llmservice.h L11 已改为 `class MemoryManager;` 前向声明）
- [ ] 创建ShortcutManager全局快捷键管理
- [x] 播放器工厂模式重构（TD-023）
- [ ] 添加错误重试机制（LLM请求失败自动重试）

**预计时间**：2-3周

### 多渠道 / 类人化路线（详见 HUMANIZATION_PLAN.md）
**状态**：Phase 0 ✅ 已完成（提交 `61f1767`）

- [x] **Phase 0 架构解耦**：`Utterance`/`RequestMeta` 统一协议、`IChannelAdapter` 渠道抽象、`SessionRouter` 输入路由、渠道适配器（桌宠/QQ 占位）
- [x] **数据库 v2 迁移**：`chat_history`/`long_term_summary` 渠道双键、`episodic_memory.origin`、`relationship_state` 按 subject 重建、`raw_messages` 建表（只建不写）
- [x] **identity.json 配置解析**（self/master/盲区/立绘跟随/主动对话，Phase 0 只读、无调度）
- [x] **Phase 0 单元测试 23 例**（memorymanager 10 / configmanager 8 / sessionrouter 5），修复 BUG-1 未摘要游标恒空、BUG-2 空标签误判
- [x] **Phase 0.5 / 记忆库 v3**（提交 `8f24ae5`）：`chat_history` 轮次语义（`user_input` 可空 + `turn_kind`/`trigger_event_ids`/`subject`）、`raw_messages`→`raw_events`、`episodic_memory` 加 `visibility`/`subject`、新建 `memory_cursor`、`appendRawEvent`/游标/过滤 API
- [x] **Phase 1 前置项（全部完成）**：
  - [x] identity 身份解析（`SessionRouter::senderId` + identity.json 启动加载）
  - [x] `PromptAssembler`（渠道可见性 C3、群聊说话人前缀、主动轮 assistant-only）
  - [x] v4 提取链路（`episodic.session_id`、提取带渠道/说话人/主体、档案三条准入规则）
  - [x] TTSService 合成/播放分离（`IAudioSink` / `PetAudioSink`，播放器归桌宠；无 sink 只合成不播放）
  - [x] GlobalMoodState（全局情绪；外部情绪驱动立绘受 D1 控制，退火归桌宠 D2）
- [ ] **Phase 1 主体（进行中：QQ 文本 + R3 记忆主体已通）**：
  - [x] SnowLuma v2.2.0 + OneBot WS 服务端 + `QQProtocolClient` 连接/重连
  - [x] 私聊/群聊文本收发、`raw_events` 落库、白名单、群聊 @ 门控、发送重试
  - [x] 记忆系统 v5 R3 主体（PR-1 ~ PR-3.2c；详见下节）
  - [ ] QQ 语音/图片/表情包输出
  - [ ] 群聊滑动窗口/发言门控
- [ ] **Phase 2**：
  - [ ] 2a 观察权 + 主人相关事件转述（只观察白名单群，只投 pet）
  - [ ] 2b 群话题摘要（主人离开后聚合）
  - [ ] 2c 非主人私聊 metadata（默认关闭）
  - [ ] 2d 主动转述反馈学习
  - [ ] `ChatPlanner` 发言门控
- [ ] **Phase 3**：QQ 空间说说/评论/点赞、合并转发/戳一戳/表情回应
- [ ] **Phase 4**：经 dsh ACP 接入浏览器类工具

**当前状态**：SnowLuma + 专用小号已部署，QQ 私聊/群聊文本链路已通；记忆 v5 R3 主体与 PR-4 MemoryAgent 已落地，下一批为 3.2b/3.2c 手工验收 + PR-5 留存/清理，QQ 语音/图片随后。

### 记忆系统 v5 R3（进行中，详见 MEMORY_V5_R3_BASELINE.md）
**状态**：PR-1 ~ PR-4 已实现；3.2a/3.2a.1 已手工验证；3.2b/3.2c 待手工验收；PR-5 未开始。

- [x] **PR-1a 安全与持久化边界**：写入前 Sanitize、群级 opt-in、短期/长期开关分离、关闭全文日志
- [x] **PR-1b 输出保护与 Prompt Injection**：记忆移入独立 `MEMORY_DATA`、`MemoryOutputGuard` 单次安全重试
- [x] **PR-2 raw_events 消费链路**：interactions / participants / safe_summary / cursor，QQ reply 关系
- [x] **PR-2.1 近期群聊上下文**：`RecentContextPlanner` + `searchRecentGroupContext`，只读、按时间窗、预算独立
- [x] **PR-2.2 Viewer Identity Facts**：`VIEWER_IDENTITY` 只提供 person_id/显示名/is_viewer 事实
- [x] **PR-3.0 Person / Person Identity**：v7 `persons` / `person_identities`，owner 与 `qq:<uin>` 归一
- [x] **PR-3.1 Summary / Claims Person IDs**：v8 `summary_format/segments`、claims 三主体 person_id、viewer-aware 渲染
- [x] **PR-3.2a 成员关系**：v9 `group_membership_periods/snapshots`、OneBot `actionResponse`、快照 fail closed
- [x] **PR-3.2a.1 成员关系生命周期**：定时刷新、入群/退群 notice、pending 去重与 10 秒超时
- [ ] **PR-3.2b Claims 授权**：四态 `MemoryAccessPolicy`、保守激活、只注入 shareable —— 已实现并通过单测，**待手工验收**
- [ ] **PR-3.2c 跨群**：Owner 允许群 / 非 Owner 共同群、viewer 过滤、非共同群 fail closed —— 已实现并通过单测，**待手工验收**
- [x] **PR-4 MemoryAgent**：只读 `memory_search` / `memory_read` / `memory_related`，QQ 记忆问句触发，超时回退 top-k，工具审计与 P95
- [ ] **PR-5 留存 UI 与生命周期**：保留期、observe-only、规则 UI、清理/删除传播 —— **下一项**

**当前基线**：数据库 `user_version=9`（14 张表）；15 个 CTest 套件 / 132 个测试函数全绿。不引入向量库。

### v0.7.0 - 架构改进 & 独立存档
**状态**：📋 待开始

**功能清单**：
- [ ] 架构改进
  - [ ] 拆分AppController职责（SignalRouter/MemoryCoordinator/ErrorHandler）
  - [ ] 消除LLMService头文件对MemoryManager的直接依赖（注：已部分修复，llmservice.h 使用前向声明，但 cpp 仍依赖）
- [ ] 独立存档系统
  - [ ] 支持多个存档
  - [ ] 每个存档有独立的记忆和角色关系
  - [ ] 存档导入/导出
- [ ] 时间驱动主动对话
  - [ ] 到饭点提醒、睡前聊天
  - [ ] 日程提醒
- [ ] 对话轮次冲突解决策略（TD-034）
  - [ ] 识别上一轮播放收尾晚于新一轮输入的情况
  - [ ] 用户可选策略：打断当前轮 / 自动进入队列
  - [ ] 修复退火误触发（仅当前轮结束时启动退火）

**技术任务**：
- [ ] 拆分AppController职责（TD-013 维持⚠️ 可接受，TimeManager 已独立为子类）
- [ ] 扩展MemoryManager支持多存档
- [ ] 创建SaveManager存档管理
- [ ] 创建ScheduleManager日程管理

**预计时间**：3-4周

### v0.8.0 - 角色自定义 & 剧本系统
**状态**：📋 待开始

**功能清单**：
- [ ] 角色自定义
  - [ ] 立绘导入
  - [ ] 角色设定配置
  - [ ] 语音模型配置
- [ ] 剧本系统
  - [ ] 剧本编辑器
  - [ ] 多角色对话支持
  - [ ] 羁绊系统
- [ ] 点击交互（不同部位触发不同回应）
- [ ] 立绘切换过渡动画（淡入淡出）

**技术任务**：
- [ ] 创建CharacterManager角色管理
- [ ] 创建ScriptEngine剧本引擎
- [ ] 立绘资源系统扩展

**预计时间**：4-5周

### v0.9.0 - 智能助手 & 性能优化
**状态**：📋 待开始

**功能清单**：
- [ ] 天气查询
- [ ] 立绘资源缓存
- [ ] TTS音频缓存
- [ ] 多角色支持
- [ ] 长期记忆增强（摘要重建机制）

**预计时间**：2-3周

---

## 技术债务清理计划

| 编号 | 债务 | 优先级 | 计划版本 | 状态 |
|------|------|--------|----------|------|
| TD-001 | API Key硬编码 | 高 | v0.3.0 | ✅ 已修复（配置文件持久化） |
| TD-005 | TTS为模拟实现 | 高 | v0.4.1 | ✅ 已修复（策略模式+GPT-SoVITS API接入完成） |
| TD-009 | 系统提示词硬编码 | 中 | v0.3.0 | ✅ 已修复（提示词外部化，prompt.txt文件） |
| TD-010 | AI状态同步机制未实现 | 高 | v0.3.0 | ✅ 已实现（状态提供者模式） |
| TD-011 | 记忆系统未实现 | 高 | v0.4.0 | ✅ 已实现（短期记忆+用户画像+长期摘要） |
| TD-012 | 长期记忆未实现 | 中 | v0.4.0 | ✅ 已实现（LLM自动提取摘要） |
| TD-016 | SUMMARY_THRESHOLD魔法数字 | 低 | v0.5.0 | ✅ 已取代（阈值改由 `memory.summary_length` 配置驱动，TD-037） |
| TD-017 | LLM标签输出格式不稳定 | 高 | v0.5.0 | ✅ 已修复（JSON协议+TagValidator校验） |
| TD-006 | 用户输入入口缺失 | 中 | v0.2.0 | ✅ 已修复（ChatWidget实现） |
| TD-002 | 立绘路径硬编码 | 中 | v0.2.0 | ✅ 已解决（AppearanceManager统一生成） |
| TD-003 | BubbleWidget位置计算硬编码 | 中 | v0.2.0 | ✅ 已解决（attachTo解耦） |
| TD-004 | 立绘切换功能未实现 | 高 | v0.2.0 | ✅ 已解决（AppearanceManager） |
| TD-007 | characterwidget大量注释代码残留 | 低 | v0.3.0 | ✅ 已清理 |
| TD-008 | appcontroller重复include | 低 | v0.3.0 | ✅ 已修复 |
| TD-013 | AppController职责过重（上帝对象） | 高 | v0.7.0 | ⚠️ 可接受（当前规模合适，TimeManager 已独立为子类） |
| TD-014 | AnchorManager内存泄漏风险 | 高 | v0.7.0 | ✅ 非问题（仅持弱引用，widget 所有权归 AppController） |
| TD-015 | LLMService头文件依赖MemoryManager | 中 | v0.7.0 | ✅ 已修复（llmservice.h 已用 `class MemoryManager;` 前向声明） |
| TD-018 | 流式PCM解析丢失数据 | 高 | v0.6.0 | ✅ 已修复（跳过WAV头透传裸PCM） |
| TD-019 | StreamPlayer野指针崩溃 | 高 | v0.6.0 | ✅ 已修复（m_buffer初始化nullptr+signals去重） |
| TD-020 | 播放完成信号不触发 | 高 | v0.6.0 | ✅ 已修复（setSynthesisDone+IdleState+兜底定时器） |
| TD-021 | 播放吞开头 | 中 | v0.6.0 | ✅ 已修复（startPlayer预填充PCM） |
| TD-022 | FilePlayer写入位置错误 | 中 | v0.6.0 | ✅ 已修复（pushData先seek到末尾再write） |
| TD-023 | TTSService直接new具体播放器 | 低 | v0.6.0 | ✅ 已修复（IPcmPlayer 静态工厂方法） |
| TD-024 | 衰减重复扣分 | 高 | v0.6.0 | ✅ 已废弃（user_profile 表整体删除，confidence/last_triggered 字段不再存在） |
| TD-025 | tier 保留逻辑反转 | 高 | v0.6.0 | ✅ 已废弃（tier 字段随 user_profile 表删除） |
| TD-026 | TTS 文本切分错误 | 中 | v0.6.0 | ✅ 已修复（cut0→cut5 按全部标点切） |
| TD-027 | 超分采样率不匹配 | 中 | v0.6.0 | ✅ 已修复（m_sampleRate 动态适配） |
| TD-028 | 角色档案 key 碎片化 | 中 | v0.6.0 | ✅ 已修复（normalizeProfileKey 带 subject 参数，服务于 character_profile） |
| TD-029 | AI 摘要盲提取 | 中 | v0.6.0 | ✅ 已修复（extractMemoryAsync 传 existingProfiles + existingMemories 增量更新） |
| TD-030 | TimeManager 野指针崩溃 | 高 | v0.6.0 | ✅ 已修复（QElapsedTimer 指针未初始化 → 重构为 QTimer singleShot） |
| TD-031 | TTSProcessManager 孤儿进程 | 高 | v0.6.0 | ✅ 已修复（apiStart 开头 killProcessOnPort 清理占端口孤儿 python.exe） |
| TD-032 | TimeManager 退火时间单位错误 | 高 | v0.6.0 | ✅ 已修复（hasExpired(15) 误为 15ms → QTimer::start(15000) 正确为 15秒） |
| TD-033 | 采样率硬编码/猜测（super_sampling 猜测 + 播放器硬编码 24000） | 中 | v0.6.0 | ✅ 已修复（流式/分段路径从服务端WAV头读取真实采样率，StreamPlayer::updateSampleRate 校正） |
| TD-034 | 对话轮冲突导致退火误触发：上一轮播放收尾（playbackQueueEmpty）晚于新一轮用户输入，被当作当前轮结束，用旧轮 finalTags 启动退火定时器，导致新一轮播放中途提前退回默认状态 | 中 | v0.7.0 | ✅ 已修复（轮次门控：`m_roundSeq` 每次输入 +1、`m_lastPlaybackRound` 记录入队轮次，仅两者相等即最新轮才触发退火） |
| TD-035 | 昼夜判定写反（跨午夜比较符用错，整个白天被误判为夜晚而穿睡衣） | 中 | v0.6.0 | ✅ 已修复（onMinuteTick 跨午夜分支改为 `(now >= m_nightStart || now < m_nightEnd)`） |
| TD-036 | 立绘 far/closer 切换漂移与闪帧（切换原点不当 + resize/move 两步重绘） | 中 | v0.6.0 | ✅ 已修复（updatePath 锚定脚底中心 + 原子 setGeometry + setPixmap 后 repaint + WA_NoSystemBackground） |
| TD-037 | 设置界面三组控件未接线：长期记忆摘轮数、夜晚时段、退火时长（按钮/控件已就位，但无槽函数、ConfigManager 无配置项、TimeManager 硬编码默认值） | 中 | v0.6.0 | ✅ 已修复（ConfigManager 新增 `memory.summary_length`/`time.*` 读写；SettingsWidget 三个保存槽；AppController `applyTimeSettings()` 启动与保存时应用） |

---

## 架构演进路线

### 阶段1：单体式（v0.1.0）
```
CharacterWidget（大而全）
    ├── LLM逻辑
    ├── BubbleWidget
    └── 立绘显示
```

### 阶段2：初步解耦（v0.1.5）
```
CharacterWidget
    ├── BubbleWidget
    └── LLMService（信号槽）
```

### 阶段3：中央控制器（v0.2.0）
```
AppController（信号调度中枢）
    ├── CharacterWidget
    ├── BubbleWidget
    ├── LLMService
    ├── TTSService
    ├── AppearanceManager
    └── ConfigManager
```

### 阶段4：AI资源管理（v0.3.0）
```
AppController（信号调度中枢）
    ├── CharacterWidget（立绘/右键菜单）
    ├── BubbleWidget（气泡/打字机）
    ├── ChatWidget（聊天输入框）
    ├── AnchorManager（位置锚点管理）
    ├── LLMService（AI对话/解析 → 状态提供者模式 + 提示词外部化）
    ├── TTSService（语音合成/播放）
    ├── AppearanceManager（四维状态 → 状态描述输出）
    └── ConfigManager（单例配置 → JSON文件持久化）
```

### 阶段5：AI记忆系统（v0.4.0）
```
AppController（信号调度中枢）
    ├── LLMService（AI对话 → JSON解析 + 记忆注入）
    │   ├── m_stateProvider → AppearanceManager.getCurrentStateDescription()
    │   ├── historyQA → MemoryManager.getHistoryTurn(15)
    │   ├── profiles → MemoryManager.getCharacterProfiles()
    │   ├── memories → MemoryManager.getActiveEpisodicMemories()
    │   ├── summaries → MemoryManager.getLatestSummaries()
    │   ├── relStates → MemoryManager.getRelationshipStates()
    │   └── extractMemoryAsync() 后台记忆提取
    └── MemoryManager（AI记忆系统 → SQLite数据库，v1 单一迁移块）
        ├── chat_history表 → 对话历史
        ├── long_term_summary表 → 长期摘要
        ├── character_profile表 → 角色档案（取代已废弃的 user_profile）
        ├── episodic_memory表 → 情景记忆（importance 衰减）
        └── relationship_state表 → 关系状态（intimacy/trust）
```

### 阶段6：TTS策略模式（v0.4.1）
```
TTSService（策略模式）
    ├── m_ttsQueue（待合成）
    ├── ITTSProvider（抽象接口）
    │   ├── ApiTTSProvider（GPT-SoVITS HTTP API）
    │   └── MockTTSProvider（模拟实现）
    └── m_playQueue（待播放）
```

### 阶段7：JSON协议 & 标签校验（v0.5.0）
```
LLMService（JSON协议）
    ├── response_format: {type: "json_object"} → 强制JSON输出
    ├── parseJsonReply() → JSON对象解析
    ├── TagValidator → 5级标签校验
    │   ├── 精确匹配
    │   ├── 大小写忽略
    │   ├── 编辑距离（Levenshtein算法）
    │   ├── 继承上一状态
    │   └── 使用默认值
    └── 历史记忆注入 → 纯文本（剥离标签格式）

数据库简化：
    chat_history表 → 仅保留 raw_reply（JSON对象），删除parsed_json列
```

### 阶段7.5：流式TTS播放 & 播放器抽象 & 时间管理（v0.6.0，当前）
```
TTSService（流式TTS）
    ├── ApiTTSProvider（streaming_mode=true）
    │   └── pcmDataReady 信号 → StreamPlayer.writePcm()
    ├── IPcmPlayer 抽象接口
    │   ├── StreamPlayer（流式PCM：QAudioSink + QBuffer）
    │   │   ├── 预填充防吞开头
    │   │   ├── setSynthesisDone() 标记合成完成
    │   │   ├── onStateChanged(IdleState) 三条件检测
    │   │   └── checkPlayEnd() 兜底定时器
    │   └── FilePlayer（文件型：QAudioSink + QFile）
    │       ├── WAV头解析（采样率+通道+位深）
    │       └── 预填充防吞开头
    ├── TTSProcessManager（GPT-SoVITS 进程管理）
    │   ├── apiStart() 开头 TCP 探测端口占用
    │   └── killProcessOnPort() 清理孤儿 python.exe（修复 SoVITS 切换 400）
    └── onPcmPlayFinished() → 清理+继续下一句

TimeManager（时间管理器，独立子类）
    ├── m_blushResetTimer（QTimer singleShot，脸红退火倒计时）
    ├── m_emotionResetTimer（QTimer singleShot，表情退火倒计时）
    ├── m_distanceResetTimer（QTimer singleShot，距离退火倒计时）
    ├── notifyRoundEnded(finalTags) → 停全部定时器；按最终状态启动：blush=blushing / emotion≠happyIdle / distance=closer
    ├── notifyUserInputStarted() → stop 三个定时器
    └── onMinuteTick() → 服装时段切换（白天校服 / 夜晚睡衣）
```

### 阶段8：插件化（v0.8.0+）
```
Core
    ├── PluginManager
    ├── ConfigManager
    ├── EventBus
    └── AppController

Plugins
    ├── CharacterPlugin
    ├── LLMPlugin
    ├── TTSPlugin
    ├── WeatherPlugin
    └── SchedulePlugin
```

---

## 依赖规划

| 依赖 | 当前版本 | 计划升级 | 说明 |
|------|----------|----------|------|
| Qt | 6.5+ | 6.11+ | Qt Multimedia已用于TTS播放 |
| CMake | 3.19+ | 3.20+ | - |
| GPT-SoVITS | - | - | v0.4.1已接入TTS引擎 |
| deepseek-v4-flash | - | - | 当前LLM模型 |

---

## 风险评估

| 风险 | 影响 | 概率 | 缓解措施 |
|------|------|------|----------|
| DeepSeek API变更 | AI对话功能失效 | 中 | LLMService封装层，便于适配 |
| LLM输出格式不一致 | 标签解析失败 | 低 | response_format强制JSON + TagValidator校验 |
| TTS服务不可用 | 语音功能失效 | 中 | 支持Api/Mock双模式切换 |
| 立绘资源过大 | 打包体积增加 | 低 | 支持资源按需加载/外部资源 |
| 网络延迟 | 用户体验下降 | 中 | 加载状态提示，TTS预合成 |
| GPT-SoVITS部署复杂 | TTS接入周期长 | 中 | 保留Mock实现，逐步替换 |

---

## 当前进度总览

```
v0.1.0  ████████████████████ 100%  基础版本
v0.2.0  ████████████████████ 100%  架构重构 & 多标签协议
v0.3.0  ████████████████████ 100%  AI资源管理
v0.4.0  ████████████████████ 100%  AI记忆系统（三层记忆）
v0.4.1  ████████████████████ 100%  TTS策略模式重构
v0.5.0  ████████████████████ 100%  JSON协议 & 标签校验
v0.6.0  ████████████████████   95%  流式TTS播放 & 播放器抽象 & 时间管理（剩外观/快捷键/托盘等）
Phase 0   ████████████████████  100%  多渠道架构解耦（已提交 61f1767）
Phase 0.5 ████████████████████  100%  记忆库 v3（已提交 8f24ae5）
Phase 1 前置 ████████████████████  100%  全部完成（identity/上下文/v4/TTS 分离/GlobalMoodState）
Phase 1    ████████████░░░░░░░░   60%  QQ 文本 + R3 记忆主体已通；语音/图片/门控待做
记忆 v5 R3 █████████████████░░░   85%  PR-1~4 已实现；3.2b/c 待手工验收；PR-5 未做
v0.7.0  ░░░░░░░░░░░░░░░░░░░░   0%  架构改进 & 独立存档
v0.8.0  ░░░░░░░░░░░░░░░░░░░░   0%  角色自定义 & 剧本系统
v0.9.0  ░░░░░░░░░░░░░░░░░░░░   0%  智能助手 & 性能优化
```

---

## 近期任务（Next Up，2026-10-29 更新）

1. **R3 封版验收**
   - 3.2b：claim 三主体激活/隔离；非 shareable 不进入 `MEMORY_DATA`
   - 3.2c：非 Owner 私聊共同群；跨群只返回 viewer 自己可披露的表达；快照失败 fail closed
   - PR-4：QQ 记忆问句触发、pet 不触发、超时回退、审计无原文
2. **PR-5 留存 UI 与生命周期**
   - 群保留期、observe-only、规则 UI、记忆清理、删除传播
3. **Phase 1 主体剩余**
   - QQ 语音/图片/表情包输出
   - 群聊滑动窗口/发言门控
4. **Phase 2 主动对话**
   - 2a 观察权 + 主人相关事件转述（只投 pet）→ 2b 群摘要 → 2c 私聊 metadata → 2d 反馈
   > 设计见 HUMANIZATION_PLAN 6.4–6.6；前置依赖 R3 验收与 scope/audience

> 顺延（不影响主线）：请求上下文隔离、盲区/敏感接线、LLM 网络重试、ShortcutManager、TTS 语速/温度、外观配置、历史导出、立绘过渡动画、系统托盘、右键菜单扩展、AppController 职责拆分（TD-013）、独立存档、插件化。
