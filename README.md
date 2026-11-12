# Windows_AI 桌面看板娘

一款基于 Qt 框架开发的桌面看板娘应用，支持 AI 对话、立绘切换、语音交互等功能。

## 📁 项目简介

这是一个可爱的桌面看板娘（桌宠）应用，名为「千岛茉子」。她可以与你对话、切换表情和服装、甚至通过 GPT-SoVITS 合成语音。

2026-11-12：M1 地基已完成，M2 跨渠道表达收尾中；M3 第一切片（文字活动/暂停恢复）已接入，待真实使用验收。当前状态与阅读顺序见 [docs/STATUS.md](docs/STATUS.md)，能力清单见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## ✨ 功能特性

### 已实现功能
- 🎭 **AI 对话**：集成 DeepSeek API，支持多轮对话（含短期记忆）
- 🎨 **立绘切换**：支持表情、服装、距离、脸红四维状态切换
- 💬 **对话气泡**：打字机特效，位置跟随角色
- 🖱️ **拖拽移动**：支持自由拖动角色窗口
- 📍 **位置锚点**：气泡和聊天窗口自动跟随角色
- ⚙️ **AI 资源管理**：配置文件持久化（JSON格式）、系统提示词外部化（prompt.txt）
- 🧠 **AI 记忆系统**（五层记忆架构 + R3 关系/授权层，SQLite数据库，`user_version=14`）：
  - 输入即时留存，按需核对跨渠道事项。历史区分渠道交接、主动安静与处理中断。
  - 短期记忆（最近N轮对话上下文，默认15轮）
  - 角色档案（character_profile，区分用户/茉子主体的稳定特质）
  - 情景记忆（episodic_memory，事件/承诺/冲突，按重要度衰减）
  - 长期记忆摘要（LLM自动提取重要事件，跨会话持久化）
  - 关系状态（relationship_state，亲密度/信任度量化）
  - 人物身份（persons / person_identities，QQ→person_id，群昵称保留有效期）
  - 群聊交互片段（interactions / interaction_participants，按会话分段并生成结构化摘要）
  - 可授权事实（memory_claims，三主体独立；默认 quarantine，保守激活）
  - 群成员关系（group_membership_periods / snapshots，定时刷新、入群/退群、失败 fail closed）
  - 近期群聊上下文（RecentContextPlanner + searchRecentGroupContext，只读按时间窗检索）
  - 主 Conversation Agent 自主调用 `memory_recall`（原生 Tool Calling；同一上下文完成工具调用与最终回复）
  - 多 need 记忆检索（lookup）；`mode=overview` 覆盖完整时间段并返回覆盖率/时间桶/可展开句柄
  - 数据源：group_recent、interaction_summary、claim、episodic、long_term_summary、chat_history
  - TimeFrame：模型表达 anchor/occurrence/span/period，服务端计算日期；公共节日数据驱动；tool 路径带时间必须提供 frame
  - Inline/Spill：档案与关系内联，历史记忆按需检索
  - `owner_cross_channel_private` 默认关闭；仅 Owner QQ 私聊可按开关读取自己的 pet/private 记忆
  - `agent_mode=legacy` 保留旧关键词门控 + 独立 Planner，作为灰度回滚
  - prompt 昵称净化（去除控制字符/双向控制符，数据库保留原始昵称）
  - 对话历史管理（分页浏览、删除、清空）
- ⚙️ **设置界面**：左侧目录 + 右侧滚动内容，点击目录跳转、滚动自动高亮双向同步；支持 API Key、短期记忆轮数、长期记忆摘轮数、夜晚时段与退火时长、TTS 模型切换、历史记录管理
- 🔌 **多渠道架构（Phase 1 主体）**：统一 `Utterance` 输出协议 + `IChannelAdapter` 渠道抽象 + `SessionRouter` 输入路由；记忆按 `channel + session_id` 双键隔离。QQ 文本已接入（SnowLuma / OneBot v11 WS + 白名单/@ 门控 + 群记忆），语音/图片/表情包待扩展

### 开发中功能
- 🔊 **语音合成**：✅ 已实现（见下方「TTS 语音系统说明」：策略模式 + 流式播放 + GPT/SoVITS 模型热切换）
- ⏰ **时间驱动**：✅ 已实现（TimeManager 按时间段自动切换服装：白天校服 / 夜晚睡衣）
- ⌨️ **全局快捷键**：开发中（支持自定义快捷键）

### TTS 语音系统说明

#### 架构设计

采用**策略模式 + 合成/播放分离**：TTSService 只负责合成，播放由渠道音频输出 `IAudioSink` 承担；桌宠渠道用 `PetAudioSink`（拥有播放器与播放队列），QQ 渠道不设 sink（只合成不播放）。

```
LLM回复文本 → enqueueSentences() → m_ttsQueue(待合成) → ITTSProvider.synthesize()
                                                                    ↓
                                    流式：pcmDataReady → IAudioSink::writePcm
                                    非流式：synthesisFinished → IAudioSink::playFile
                                                                    ↓
                                    PetAudioSink（播放列队/播放器/首块起播）
```

| 组件 | 职责 |
|------|------|
| `TTSService` | 合成队列、Provider 管理、把音频交给 `IAudioSink` |
| `IAudioSink` / `PetAudioSink` | 音频输出抽象 / 桌宠播放实现（播放器 + 播放队列 + 气泡时序） |
| `ITTSProvider` | 抽象接口，支持多种 TTS 实现 |
| `ApiTTSProvider` | 通过 HTTP API 调用 GPT-SoVITS 服务，流式合成时从 WAV 头读取真实采样率 |
| `MockTTSProvider` | 模拟实现（开发测试用） |
| `StreamPlayer` | 流式 PCM 随到随放播放（QAudioSink + QBuffer） |
| `FilePlayer` | 文件型 WAV 播放（解析文件头采样率并预填充） |

#### TTS 模式切换

在配置文件 `setting.json` 中设置：
```json
{
    "tts": {
        "mode": "api"
    }
}
```

| 模式 | 说明 |
|------|------|
| `api` | 使用 GPT-SoVITS API 真实合成语音 |
| `mock` | 模拟实现，不发起网络请求 |

#### 参考音频配置

GPT-SoVITS 需要参考音频文件（3-10秒），根据角色情绪状态映射：

| 情绪 | 参考音频文件 |
|------|-------------|
| happyIdle | happy_idle_01.wav |
| happyMore | happy_more_01.wav |
| amazing | amazing_01.wav |
| loving | loving_01.wav |
| caring | caring_01.wav |
| sad | sad_01.wav |
| conscientious | conscientious_01.wav |

**参考音频要求**：
- 格式：WAV（16bit, 32kHz 或 48kHz, mono）
- 时长：建议 3-10 秒（GPT-SoVITS API_V2 限制 ≤10秒）
- 存放路径：`app_data/reference_audio/`

#### GPT-SoVITS 部署

1. 克隆 GPT-SoVITS 项目并配置模型
2. 启动 API 服务：
```bash
python api_v2.py -a 127.0.0.1 -p 9880
```
3. 确认服务正常：访问 `http://127.0.0.1:9880/docs`

**API 请求示例**：
```json
POST /tts
{
    "text": "こんにちは、千島茉子です",
    "text_lang": "ja",
    "ref_audio_path": "path/to/reference.wav",
    "prompt_text": "参考音频对应的文本",
    "prompt_lang": "zh",
    "text_split_method": "cut5",
    "media_type": "wav",
    "streaming_mode": true
}
```

> **采样率说明**：流式模式下客户端从服务端返回的首个 WAV 头（偏移 24）读取真实采样率（v3=24000Hz，v1/v2=32000Hz，v4/超分=48000Hz），不再按 `super_sampling` 硬编码；非流式文件播放由 FilePlayer 解析文件头自适。v3 模型的"流式"实为句子级分段返回（首音需等首句整句合成完成）。

### 计划功能（未完成；当前顺序以 STATUS/ROADMAP 为准）
- 🔌 **QQ 语音/图片/表情包**：Phase 1 剩余（文本已通；协议端 SnowLuma，Qt 作为 WS 客户端）
- 🔧 **架构改进**：拆分 AppController 职责（信号路由、记忆协调、错误处理）
- 📁 **独立存档系统**：支持多个存档，每个存档有独立的记忆和角色关系
- ⏰ **时间驱动主动对话**：到饭点提醒、睡前聊天等
- 🎯 **点击交互**：不同部位触发不同回应
- 🎬 **过渡动画**：立绘切换淡入淡出效果
- 🎭 **角色自定义**：支持用户导入立绘和自定义角色设定
- 🌤️ **智能助手**：天气查询、日程提醒等
- 👥 **多角色支持**：可切换不同角色

## 项目文档

安装和操作说明见本文。当前架构、配置、记忆接口、开发任务及历史记录从 [文档索引](docs/README.md) 进入。

## 🛠️ 技术栈

- **框架**: Qt 6.5+
- **语言**: C++17
- **构建系统**: CMake 3.19+
- **AI 服务**: DeepSeek API (deepseek-v4-flash)
- **语音合成**: GPT-SoVITS（API 已接入，策略模式封装）
- **音频播放**: IPcmPlayer 抽象（StreamPlayer 流式 / FilePlayer 文件，QAudioSink + QBuffer）
- **数据库**：SQLite（user_version=14）。会话、认知来源、事项修订和历史依据的结构见 [架构与当前实现](docs/ARCHITECTURE.md)。

## 📦 安装与构建

### 环境要求

- Windows 10/11
- Qt 6.5+（带 Qt Creator 或 qmake）
- CMake 3.19+
- Visual Studio 2019/2022 或 MinGW 编译器

### 构建步骤

#### 方法一：使用 Qt Creator（推荐）

1. 打开 Qt Creator
2. 选择「打开项目」，选择项目根目录的 `CMakeLists.txt`
3. 配置 Kit（确保 Qt 6.5+，选择合适的编译器）
4. 点击「构建」按钮
5. 构建成功后，点击「运行」按钮

#### 方法二：使用命令行

```bash
# 1. 克隆项目
git clone https://github.com/your-username/Windows_AI.git
cd Windows_AI

# 2. 创建构建目录
mkdir build
cd build

# 3. 配置 CMake（替换为你的 Qt 安装路径）
cmake .. -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/mingw_64"

# 4. 构建项目
cmake --build . --config Release

# 5. 运行
./Release/Windows_AI.exe
```

### 依赖项

项目使用以下 Qt 模块：
- Qt::Core - 核心功能
- Qt::Widgets - UI组件
- Qt::Network - 网络请求（DeepSeek API、GPT-SoVITS API）
- Qt::Sql - SQLite数据库（对话历史）
- Qt::Multimedia - 音频播放（QAudioSink 低延迟 PCM 播放）

## ⚙️ 配置

### 配置文件路径

首次运行后，配置文件位于：`app_data/config/setting.json`

### 配置项说明

> `setting.json` 是纯 JSON，不支持 `//` 或 `/* */` 注释；设置界面保存时会按固定字段重写文件。完整字段、默认值与取值范围见 [CONFIG_REFERENCE.md](docs/CONFIG_REFERENCE.md)。

```json
{
    "api": {
        "deepseek_api_key": "your-api-key-here",
        "gpt_sovits_url": "http://127.0.0.1:9880"
    },
    "memory": {
        "short_term_length": 15,
        "summary_length": 15,
        "short_context_enabled": true,
        "long_term_memory_enabled": true,
        "debug_full_logs": false,
        "agent_enabled": true,
        "agent_mode": "tool",
        "agent_protocol": "native",
        "owner_cross_channel_private": false,
        "agent_plan_v2": true,
        "agent_trigger": "memory_question",
        "agent_timeout_ms": 6000
    },
    "time": {
        "night_start": "22:00",
        "night_end": "07:00",
        "blush_lasting": 30,
        "emotion_lasting": 15,
        "distance_lasting": 15
    },
    "qq": {
        "enabled": false,
        "ws_url": "ws://127.0.0.1:3001",
        "access_token": "",
        "reply": {
            "private": "master",
            "group": "none"
        }
    },
    "tts": {
        "mode": "api"
    }
}
```

| 配置组 | 关键字段 | 说明 |
|--------|----------|------|
| `api` | `deepseek_api_key` / `gpt_sovits_url` | DeepSeek 密钥、GPT-SoVITS 服务地址 |
| `memory` | `short_term_length` / `summary_length` / `short_context_enabled` / `long_term_memory_enabled` | 短期上下文与长期记忆分别控制；关闭长期记忆不等于关闭短期上下文 |
| `memory` | `debug_full_logs` / `privacy_rules` | 默认关闭全文日志；隐私规则支持 exact / keyword / pattern |
| `memory` | `recent_group_context` | 近期群聊检索预算；默认开启 |
| `memory` | `agent_enabled` / `agent_mode` / `agent_protocol` | `tool`：主模型自主调用 `memory_recall`；`legacy`：旧关键词门控 + 独立 Planner；协议可选 `native` / `json_envelope` |
| `memory` | `owner_cross_channel_private` | 默认 `false`；仅 Owner QQ 私聊可读取自己的 pet/private 记忆，群聊/非 Owner 始终禁止 |
| `memory` | `agent_timeout_ms` / `agent_max_rounds` / `agent_max_results` | 工具超时、工具轮数与单次 need 结果上限 |
| `time` | `night_start` / `night_end` / `*_lasting` | 昼夜切换与表情/脸红/距离退火时长 |
| `qq` | `enabled` / `ws_url` / `access_token` / `reply` | OneBot v11 WS 连接与私聊/群聊回复策略 |
| `qq` | `group_memory_*` / `group_sender_allowlist` / `group_retention_days` | 群记忆 opt-in、群模式、记录/回复白名单与保留期 |
| `qq` | `membership_refresh_minutes` / `membership_max_age_minutes` | 成员快照刷新间隔（0=关闭）与过期时间；过期后跨群 fail closed |
| `tts` | `mode` / 模型路径 / 参考音频 | `api`（真实合成）或 `mock`（模拟） |

### identity.json（身份绑定，Phase 1 前置，已实现）

路径：`app_data/config/identity.json`（不存在时使用默认配置，桌宠行为不变）

```json
{
  "identity": {
    "self":   { "qq": ["10001"] },
    "master": { "qq": ["20001"] }
  },
  "appearance": { "follow_external_mood": true }
}
```

| 字段 | 说明 |
|------|------|
| `identity.self.qq` | 她自己的 QQ（用于丢弃自触发消息） |
| `identity.master.qq` | 主人的 QQ 列表（多账号都归一为 `subject='user'`） |
| `appearance.follow_external_mood` | D1：外部渠道（QQ）的情绪是否驱动桌宠立绘；默认 `true`，`false` 时只更新全局情绪 |

规则：按 uin 绑定（与昵称无关）；`self` 消息直接丢弃、`master` → `user`、未知 QQ → `qq:<uin>`；应用启动时加载，日志输出 `identity.json 加载完成: self N 个 / master N 个`。

可选项：`memory.blind_zones`（不入长期记忆的渠道/会话）与 `proactive.*`（空闲念头回顾及配置目标的草稿分享，默认关闭）也在该文件中配置，详见 [CONFIG_REFERENCE.md](docs/CONFIG_REFERENCE.md)。

### QQ 接入（可选）

SnowLuma 提供 OneBot v11 WebSocket 服务端；在 `setting.json` 的 `qq` 段填写 `ws_url`、`access_token`，并用 `reply` 白名单控制哪些好友/群可回复。默认 `strict_whitelist` 下未通过白名单不记录、不回复；显式开启 `group_authorized` 后，非白名单群成员可进入群记忆，但不获得回复资格。字段与策略见 [ARCHITECTURE.md](docs/ARCHITECTURE.md) 的「QQ 渠道」与 [CONFIG_REFERENCE.md](docs/CONFIG_REFERENCE.md)。

### 获取 DeepSeek API Key

1. 访问 [DeepSeek Platform](https://platform.deepseek.com/api_keys)
2. 注册/登录账号
3. 创建 API Key

### 系统提示词

系统提示词文件位于：`app_data/config/prompt.txt`

首次启动时，会从资源文件自动释放默认提示词。你可以修改此文件来自定义茉子的性格和行为。

## 📖 使用指南

### 基本操作

1. **启动应用**：运行 `Windows_AI.exe`，茉子会出现在桌面上
2. **拖动移动**：鼠标左键拖动茉子可以移动位置
3. **右键菜单**：右键点击茉子可以打开菜单（聊天/设置/退出）
4. **聊天**：选择「和茉子聊天」打开聊天窗口，输入内容后按回车发送
5. **设置**：选择「设置」打开设置界面，配置 API Key、短期记忆轮数和 TTS 模型

### 对话历史管理

在设置界面的「历史会话记录」区块中：
- 查看历史对话记录（分页浏览）
- 删除选中的记录（需要API Key验证）
- 清空所有记录（需要API Key验证）

### 立绘状态说明

茉子的立绘支持四维状态切换：

| 维度 | 可选值 | 说明 |
|------|--------|------|
| emotion | happyIdle, happyMore, amazing, loving, caring, sad, conscientious | 表情状态 |
| blush | unblushing, blushing | 脸红状态（自动退热） |
| distance | far, closer | 距离远近 |
| clothing | pajama, schoolUniform, schoolUniformWithoutCap, schoolUniformWithoutCoat | 服装类型 |

### 记忆系统说明

茉子的记忆系统保留五层蒸馏结构（短期记忆 / 角色档案 / 情景记忆 / 长期摘要 / 关系状态），并在 R3 增加“身份、交互、事实授权、成员关系”四层可追溯结构：

| 记忆类型 | 存储方式 | 描述 |
|----------|----------|------|
| **短期记忆** | SQLite（chat_history） | 最近N轮对话（默认15轮），作为AI对话上下文 |
| **角色档案** | SQLite（character_profile） | 用户/茉子的长期稳定特质（subject/key/value，区分主体） |
| **情景记忆** | SQLite（episodic_memory） | 事件/承诺/冲突/里程碑，按重要度衰减 |
| **长期摘要** | SQLite（long_term_summary） | LLM自动提取的重要事件摘要，跨会话持久化 |
| **关系状态** | SQLite（relationship_state） | 亲密度/信任度量化（默认30/30），驱动角色关系 |
| **人物身份** | SQLite（persons / person_identities） | owner / `qq:<uin>` 的稳定 person_id；群昵称保留 valid_from/valid_to |
| **群聊交互** | SQLite（interactions / interaction_participants） | 按会话和时间窗分段的交流片段；摘要带 person_refs segments |
| **可授权事实** | SQLite（memory_claims） | 一条 claim 只表达一个事实；默认 quarantine，仅当前群三主体一致的自述可保守激活 |
| **成员关系** | SQLite（group_membership_periods / snapshots） | 快照成功才开区间；入群/退群维护区间；失败保留旧区间但跨群 fail closed |

**记忆提取机制**：当未摘要对话达到「长期记忆自动摘轮数」（默认 15 轮，可在设置界面或 `memory.summary_length` 配置）时，自动触发后台记忆提取，LLM 分析对话内容，传入已有档案做增量更新，生成角色档案、情景记忆、长期摘要和关系状态更新。

**存储分层（v9）**：`raw_events` 存全量原始事件（QQ / B 站 / 窥屏 / 主动触发；`message_id` 去重，默认不进上下文；带 `redaction_status` / `redaction_rules` / `policy_version` / `person_id`）；`chat_history` 存对话轮次（`turn_kind` = reply/proactive/perception，`user_input` 可为 NULL，`trigger_event_ids` 关联原始事件，`subject` 标记对话对象）；`interactions` 由 `RawEventProcessor` 按会话和时间窗消费 `raw_events` 生成，`interaction_participants` 保留参与者 person_id；`episodic_memory` / `long_term_summary` / `character_profile` / `relationship_state` 存蒸馏产物（情景记忆带 `origin` / `visibility` / `subject` / `session_id`，按渠道+会话隔离）；`memory_claims` 存可独立授权的长期事实；`group_membership_*` 存成员区间与快照状态。`memory_cursor` 按 `channel + session_id` 记录蒸馏进度。提取时按「长期有效 / 本人明确陈述 / 非敏感」决定是否提升为角色档案。

**R3 披露规则**：MemoryAccessState 定义四个状态，当前 evaluate 实际返回 Shareable 或 Forbidden。既有披露工具只注入可披露事实，并检查当前群、主体、成员关系和来源。新增内部证据路径见 [架构](docs/ARCHITECTURE.md)，早期设计见 [R3 历史基线](docs/MEMORY_V5_R3_BASELINE.md)。

**时间检索与文本安全**：v2 Planner 先拆多个 recall need，`MemoryTimeResolver` 独立解析“昨晚 / 上周三晚上 / 今天下午”等表达；`MemoryExecutor` 负责去重、partial 和权限。旧 `MemoryTimeRangePlanner` 仅作 fallback。昵称进入 prompt 前会去除控制字符、双向控制符和零宽字符，但数据库保留原始昵称；受异常昵称影响的失败摘要会在启动时按需重新排队。

## 📁 项目结构

- 仓库根目录：Qt/C++ 源码、CMakeLists.txt 与资源清单。
- image/、calendar/：角色与日历资源。
- tests/：Qt Test 与 CTest。
- docs/：架构、配置、协议、路线图和历史。
- build/：构建产物；其中 app_data/ 保存对应可执行文件使用的配置、记忆和日志。

具体职责见 [架构](docs/ARCHITECTURE.md)，文档目录见 [索引](docs/README.md)。

## 📡 API 参考

### DeepSeek API

项目使用 DeepSeek v4 Flash 模型进行 AI 对话。

**请求格式**：
- **Endpoint**: `https://api.deepseek.com/chat/completions`
- **Method**: POST
- **Content-Type**: application/json
- **Authorization**: Bearer {api_key}

**请求参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| model | string | 模型名称，固定为 "deepseek-v4-flash" |
| messages | array | 消息数组，包含系统消息、历史对话和用户输入 |
| temperature | float | 温度参数，默认 0.7 |

**JSON 输出协议**（AI回复格式，v0.5.0 起替代旧的多标签格式）：
```json
{"sentences": [{"zh_text": "中文", "ja_text": "日文", "tags": {"emotion": "happyIdle", "blush": "unblushing", "distance": "far", "clothing": "schoolUniform"}}]}
```

### GPT-SoVITS API

项目通过 HTTP API 集成 GPT-SoVITS 语音合成引擎。

**请求格式**：
- **Endpoint**: `http://127.0.0.1:9880/tts`
- **Method**: POST
- **Content-Type**: application/json

**请求参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| text | string | 待合成文本 |
| text_lang | string | 文本语言（zh/ja/mix） |
| ref_audio_path | string | 参考音频文件路径 |
| prompt_text | string | 参考音频对应的文本 |
| prompt_lang | string | 参考音频语言 |
| text_split_method | string | 文本切分方法（cut0/cut5等） |
| media_type | string | 输出格式（wav/ogg） |
| streaming_mode | bool | 是否流式传输 |

**响应**：直接返回音频文件二进制流

**错误码**：
| 状态码 | 原因 | 解决方案 |
|--------|------|----------|
| 400 | 参数错误/参考音频不存在/时长超限 | 检查文件路径、确认音频≤10秒 |
| 404 | 模型文件不存在 | 检查模型路径配置 |
| 500 | 服务内部错误 | 查看服务端日志 |

### GPT-SoVITS API（详细文档）

参见 [GPT-SoVITS 参考](docs/GPT_SOVITS_REFERENCE.md)。

## ⚠️ 已知限制

1. **流式粒度为句子级**：v3 模型"流式"实为句子级分段返回，首音需等首句整句合成完成（token 级真流式仅 v1/v2 + `streaming_mode=2/3` 支持）
2. **网络失败无自动重试**：TTS/网络请求失败后不会无限重试；LLM 的空回复/输出保护有单次重试，QQ 发送 `retcode=100` 有 4 秒单次重试
3. **无网络状态检测**：应用启动时不会检测网络连接状态
4. **立绘切换无过渡动画**：换图时直接切换，没有淡入淡出效果
5. **仅支持单角色**：当前仅支持「千岛茉子」一个角色
6. **外部渠道**：QQ 文本、群记忆、成员关系已接入；QQ 语音/图片/表情包、群聊滑动窗口与主动转述待后续，具体范围见 [路线图](docs/ROADMAP.md)
7. **旧 R3 尾巴**：claim 三主体激活与跨群 viewer 过滤已通过单测、待真实群验收；legacy MemoryAgent 与 Memory Recall Tool 已实现；留存 UI 尚未开始（建议并入生活引擎的管理员面板）

## 🔧 故障排除

### 问题1：配置文件损坏

**现象**：启动时日志显示 `[ConfigManger]:配置文件损坏`

**解决方案**：
- 删除 `app_data/config/setting.json` 文件
- 重新启动应用，会自动创建新的配置文件
- 或手动编辑配置文件，确保是标准 JSON；**不要加 `//` 或 `/* */` 注释**，Qt 的 JSON 解析器会拒绝
- 修改前建议备份；设置界面保存会按固定字段重写整个文件

### 问题2：API Key 无效

**现象**：发送消息后没有回复，日志显示网络错误

**解决方案**：
1. 检查 `app_data/config/setting.json` 中的 API Key 是否正确
2. 确认 API Key 没有过期或被禁用
3. 在设置界面重新输入并保存 API Key

### 问题3：数据库连接失败

**现象**：日志显示 `[MemoryManager]数据库打开失败`

**解决方案**：
1. 检查 `app_data/memory/` 目录是否存在
2. 确保应用有读写该目录的权限
3. 删除 `app_data/memory/QianDaoMoZi_memory.db` 文件后重新启动

### 问题4：立绘不显示

**现象**：窗口空白，没有显示立绘

**解决方案**：
1. 检查 Qt 资源文件 `image.qrc` 是否正确配置
2. 确认立绘图片文件存在于 `image/` 目录中
3. 检查图片路径是否正确（格式：`:image/{distance}/{clothing}/{blush}/{emotion}.png`）

### 问题5：聊天窗口不弹出

**现象**：右键菜单选择「和茉子聊天」后没有反应

**解决方案**：
1. 检查 `ChatWidget` 是否正确注册到 `AnchorManager`
2. 确认 `CharacterWidget::chatRequested()` 信号已连接到 `ChatWidget::popup()`

### 问题6：气泡位置不正确

**现象**：气泡显示位置与角色不匹配

**解决方案**：
1. 检查 `AnchorManager::calculatePosition()` 中的锚点策略计算
2. 确认 `CharacterWidget::getVisibleRect()` 正确计算了立绘有效区域

### 问题7：TTS 返回 400 Bad Request

**现象**：日志显示 `[ApiTTS]:网络请求失败 "server replied: Bad Request"`

**解决方案**：
1. **检查参考音频文件是否存在**：
   ```powershell
   Test-Path "app_data/reference_audio/conscientious_01.wav"
   ```
2. **检查参考音频时长**（需 ≤10 秒）：
   ```powershell
   # 估算时长（32kHz采样率）
   (Get-Item "app_data/reference_audio/loving_01.wav").Length / 64000
   ```
3. **检查文件名是否匹配**：参考音频文件名需与情绪对应表一致（如 `conscientious_01.wav`）
4. **查看 GPT-SoVITS 服务端日志**获取具体错误信息

### 问题8：TTS 合成失败被跳过

**现象**：日志显示 `[TTS]合成失败，跳过`

**解决方案**：
1. 检查 GPT-SoVITS 服务是否正常运行
2. 确认 `gpt_sovits_url` 配置正确
3. 查看服务端日志获取详细错误信息

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📄 许可证

MIT License

## 📧 联系方式

如有问题或建议，欢迎提交 Issue。

---

> 千岛茉子正在期待与你相遇 ❤️
