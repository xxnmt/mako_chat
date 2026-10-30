# 类人化升级方案（茉子 → 拥有自己的社交生活）

> 项目：Windows_AI 桌面看板娘（桌宠）
> 类型：规划文档（分阶段实施中）
> 版本：v1.3（Phase 1 主体 QQ 文本 + 记忆 v5 R3 + PR-4 MemoryAgent，2026-10-29）
> 制定日期：2026-10-19
> 状态：Phase 0 / 0.5 已完成；Phase 1 前置项全部完成；Phase 1 主体 **QQ 私聊/群聊文本收发已通**（SnowLuma + OneBot WS + 白名单/@ 门控）；记忆 v5 R3 主体与 PR-4 MemoryAgent 已实现（PR-1~4，3.2b/3.2c 待手工验收，PR-5 未开始）；下一步 R3 验收、PR-5 留存 UI、QQ 语音/图片与 Phase 2 主动对话
> 关联文档：[ROADMAP.md](ROADMAP.md)、[ARCHITECTURE.md](ARCHITECTURE.md)

---

## 一、升级目标

她不再只是一个桌宠，而是更贴合"人"的概念：

1. **原有桌宠功能全部保留**（气泡对话、立绘四维状态、GPT-SoVITS 语音、五层记忆）
2. **拥有自己的 QQ**：好友聊天、群聊聊天、发送表情包、发送语音、发布 QQ 空间动态
3. **能够主动发起对话**（状态/时间驱动，而非被动应答）
4. **能够使用其他社交或媒体软件**（浏览器类优先，微信/官方 API 暂缓）

---

## 二、已确认的关键决策

| 决策项 | 结论 | 备注 |
|---|---|---|
| 角色大脑归属 | **Qt 自研为主线**（LLMService + 五层记忆 + 立绘 + GPT-SoVITS 全部保留） | dsh 不承载核心人格资产 |
| QQ 技术路线 | **路线 A：Qt `QWebSocket` 客户端连接协议端 WS 服务端（OneBot v11）**；Phase 4 再将 dsh 作为"外挂工具执行器"接入 | 协议端选型见 3.2 |
| QQ 账号 | 专用小号；好友和群聊由用户登录 QQ 号手动管理添加 | 不主动加陌生人/陌生群 |
| 身份策略 | **公开 AI 身份**（在签名/资料/必要时坦承） | 合规风险显著降低 |
| 主动对话触发 | **Qt 定时器 + 状态/时间驱动**，主动权始终由 Qt 持有 | dsh 三条路径均不支持 agent 主动发起 |
| 主动频率 | **做成可配置项**（字段设计见第六节，参考 dsh 社区已验证项目） | 用户已拍板 |
| 其他社交软件 | 浏览器类（Playwright MCP）优先；先只做 QQ，其他暂缓 | 用户已拍板 |
| 实施节奏 | 先定方案，再实施；Phase 0 程度待定 | 用户已拍板 |

---

## 三、调研结论摘要

### 3.1 DSH（DeepSeek Harness）分析

**本质**：DeepSeek 官方 Agent Harness（Electron GUI + 内置 Node 24 / Python 3.12 runtime），MIT 协议、developer preview。本地版本 `0.2.0-rc.2`、nightly 通道（`https://download.deepseek.com/dsh-desk/feeds/win-x64/`）。它不是模型 API，而是**完整的 agent 运行时**：工具循环、MCP 客户端、skills、subagent、沙箱 shell、web search、schedule 定时器。

**本地已核实事实**：
- `dsh` CLI 可直接调用（`D:\DSH\resources\runtime\cli\bin\dsh.cmd`）
- profile 机制：`~/.dsh/profiles/<name>`（`package.json` 声明 bundle 列表 + `cordis.patch.yml` 用户补丁层）
- 当前仅有 `desktop` profile（= `dsh-base` + `dsh-web-app`），DeepSeek 账号已登录（provider = `deepseek-account`，模型 `deepseek-flash`，reasoningEffort = high）

**四条集成路径**：

| 路径 | 形式 | 适合场景 | 短板 |
|---|---|---|---|
| `--profile headless` | 一次性子进程 | 脚本化单任务（`--json` 行事件流，`--session-id` 续接） | 无交互后续 |
| `--profile sdk` | stdio JSON-RPC 长驻 | 协议面最小（`initialize`/`session/prompt`/`shutdown` + 4 通知），Qt 实现成本最低 | 无会话 list/resume、无取消 |
| `--profile acp` | stdio ACP v1 长驻 | **多会话、跨进程恢复、可挂 MCP、有权限交互 ← 最适合"长驻角色大脑"** | 协议较复杂 |
| `dsh-mcp-client` 反向 | DSH 调用我们的工具 | 让 agent 获得"发 QQ 消息"等私有能力 | 需在 profile 补丁层配置 |

**关键缺口**：三条路径**都不支持 agent 主动发起消息**——主动权必须由外部持有（Qt 定时器到点发 prompt）。

**第三方验证**：`dsh-acp-server` + Rust 客户端 Martty 已走通"外部程序驱动 dsh"之路（[ACP Clients 列表](https://agentclientprotocol.com/get-started/clients)）。

### 3.2 QQ 接入路线对比

| 路线 | 能力 | 代价 |
|---|---|---|
| **NapCat / SnowLuma**（OneBot v11 私有协议端） | 私聊·群聊·图片·表情包·语音·合并转发·戳一戳·表情回应·空间说说 | 注入 NTQQ，**有封禁风险**，需小号；NapCat 许可证禁商用 |
| **官方 QQ 机器人平台** | 群聊/单聊、图片、语音 | **主动消息每月各仅 4 条**、**无空间说说**、需提审——与"主动撩人"根本冲突 |

**协议端选型（2026-10 修订，推翻旧结论）**：

| 能力 | NapCat | SnowLuma | Lagrange.OneBot | LLOneBot |
|---|---|---|---|---|
| 基础聊天（私聊/群聊/图片/语音/表情包/合并转发/戳一戳/表情回应） | ✅ 全 | ✅ 全 | ✅ 全 | ✅ 全 |
| 发说说（`send_qzone_msg`） | ✅ | ✅ | ⚠️ 部分 | ❌ |
| 说说评论/点赞 | ❌ 无内置（需第三方 qzone-mcp） | ✅ 内置（专设"空间"分组） | ❌ | ❌ |
| Windows 原生（无 Docker/WSL） | ✅ | ✅ 更自动化（自动注入 QQ.exe） | ⚠️ 需 .NET 运行时 | ⚠️ 需 LiteLoader 框架 |
| WebUI 管理 | ✅ | ✅ 更强（5099 端口，调试价值高） | 简单 | 弱 |
| MCP 接入 | 第三方 | ✅ 原生（与 Phase 4 dsh 方向一致） | ❌ | ❌ |
| 生态成熟度 | 中文社区最活跃 | 较新（2024 起步），文档完整 | 活跃 | 一般 |

**推荐：SnowLuma**。理由：① Phase 3 空间互动（评论/点赞）内置，NapCat 需第三方 MCP 拼装；② Windows 原生部署体验更好；③ 原生 MCP 与 Phase 4 的 dsh 集成方向一致；④ WebUI 可作 QQChannelAdapter 调试观察台。**唯一风险**：项目较新、长期维护风险略高——但协议端可插拔，换端成本只是改连接配置。

**旧结论修正**：NapCat 并非"无评论/点赞"——`send_like` ✅ 支持点赞，但空间评论需第三方扩展，深度不如 SnowLuma 内置。

**语音技术要点**：QQ 语音为 silk v3 格式，但 **NapCat 发送时自动把 wav/mp3 转 silk**，C++ 端无需自备编码器，直接把 GPT-SoVITS 输出的 wav 塞进 `record.file` 即可。

### 3.3 相似开源项目

| 项目 | Star | 技术栈 | 参考价值 |
|---|---|---|---|
| [MaiBot 麦麦](https://github.com/Mai-with-u/MaiBot) | 5.4k+ | Python 3.12/FastAPI | **QQ 侧最成熟**：统一连接器（SnowLuma/NapCat）、`proactive.trigger` 主动私聊、`reply_timing`（frequency/dynamic）发言时机、A-Memorix 分层记忆、群记忆隔离 |
| [AstrBot](https://github.com/AstrBotDevs/AstrBot) | 36k+ | Python | 多平台适配器 + Proactive Agent（`future_task` 工具 + Cron + UMO 投递定位） |
| [Super Agent Party](https://github.com/heshengtao/super-agent-party) | 2.7k | Python/Docker | **唯一同时具备"桌宠 UI + QQ/微信 + 语音口型"** 的一体化实现，内置 GPT-SoVITS，架构最值得参考 |
| [Open-LLM-VTuber](https://github.com/Open-LLM-VTuber/Open-LLM-VTuber) | — | Python + Live2D | idle 阈值 → 主动说话 的参考实现（`idle_timeout` 与主动说话阈值是两个不同参数，勿混用） |
| NoneBot2 / Koishi | 7.7k | Python / Node | 纯接入中间件，对纯 C++ 项目反而是负担，不采用 |

### 3.4 "大肥鱼"系项目（已核实真实存在）

"大肥鱼" = DeepSeek 蓝鲸 Logo 的拟人化二创形象。B站评论区所谓"流窜的大肥鱼AI"**未找到确切项目**（B站评论区 AI 走 Cookie+Web 协议逆向，无官方评论 API，风控严、不建议参考）。但 GitHub 上"大肥鱼"桌宠系项目真实且活跃：

| 项目 | Star | 技术栈 | 与本项目的关联 |
|---|---|---|---|
| [PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet) | 2146 | TS | DSH 内置桌宠，可独立透明置顶窗，100+动画、碎碎念 ← **形态最接近本项目** |
| [MerZlin/dsh-pet-indesktop](https://github.com/MerZlin/dsh-pet-indesktop) | 1229 | **Python+PySide6** | **脱离 DSH 的独立桌宠**（Qt 系），主动识屏 |
| [1190fasheqi/dafeiyu-pet](https://github.com/1190fasheqi/dafeiyu-pet) | 216 | Python | "鲸鱼娘·大肥鱼"二创 |
| [Sutera-Diffusus/dsh-whale-musume](https://github.com/Sutera-Diffusus/dsh-whale-musume) | 141 | — | 90+立绘、530+台词、主动闲聊机制 |
| [Jimmy0123-ux/dsh-token-pet](https://github.com/Jimmy0123-ux/dsh-token-pet) | 56 | TS/React | 悬浮 Token 宠物 |
| [GarfieldZhung/DeepSeek-Whale-Girl](https://github.com/GarfieldZhung/DeepSeek-Whale-Girl) | 10 | Electron | 桌面悬浮"大肥鱼"女仆 |
| ~~MeteorNOX/DeepSeek-Balance-Whale-Girl~~ | — | — | ❌ 不存在（404） |

### 3.5 dsh 社区主动对话插件（字段设计的直接来源）

| 项目 | 状态 | 可借鉴字段 |
|---|---|---|
| [john-walks-slow/dsh-proactive](https://github.com/john-walks-slow/dsh-proactive) | ✅ 真实 | `at`/`after_seconds`/`every_seconds`/`cron`/`schedule_file`、`jitter_seconds`、`min_idle_seconds`、`respect_quiet_hours`、`target_mode` |
| [Kanadego/dsh-heartbeat](https://github.com/Kanadego/dsh-heartbeat) | ✅ 真实（v2.0.0，Windows only） | "分寸闸门"= 静默时段/忙时窗口/每日上限/开口冷却/在场联动；默认 60 分钟一跳 |
| [PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet) | ✅ 真实 | `whisperEnabled`（主动周期说话，默认开）+ `eventsRefreshSec.whisper`（默认 300 秒） |
| [Sutera-Diffusus/dsh-whale-musume](https://github.com/Sutera-Diffusus/dsh-whale-musume) | ✅ 真实 | 主动闲聊 5–8 分钟、深夜 23:00–5:59 不打扰、关怀间隔 ≥15 分钟、话痨 15–40s / 安静 90–240s |
| 官方 `@deepseek-ai/dsh-schedule` | ✅ 真实 | 仅支持 `after_seconds`/`at`/`every_seconds`（最小 300 秒），**不支持 cron/daily/weekly**，参考价值低于社区插件 |
| [beijingwahw/dsh-proactive](https://github.com/beijingwahw/dsh-proactive) | ✅ 真实（★5） | 重型自主调度（双心智/能量共生），过于复杂，不采用 |

**QQ/OneBot 相关 dsh 插件**（仅作参考，本项目走 Qt 自研）：
- [cheesehaqi/dsh-qq-onebot-bridge](https://github.com/cheesehaqi/dsh-qq-onebot-bridge)（★7）：OneBot v11，兼容 NapCat/Lagrange/LLOneBot
- npm `dsh-onebot-qq`/`dsh-onebot-hub`：反向 WS 默认端口 8643
- 注意：`@tencent-connect/dsh-qqbot`（★145）走的是 **QQ 官方机器人平台，与 NapCat 不互通**

**dsh 生态规模**：[awesome-dsh-plugin](https://github.com/awesome-dsh-plugin/awesome-dsh-plugin) 收录 4765 个插件、23 个分类（⚠️ 数据来自第三方聚合站，官方对 core plugins/API 明言"持续演进"，接入前须自行审源码）。

### 3.6 其他社交软件可行性

| 路径 | 可行度 | 说明 |
|---|---|---|
| 浏览器类（Playwright MCP / Chrome DevTools MCP） | **高** | `npx` 直接挂到 dsh 的 `mcp-client`，可驱动已登录浏览器 |
| B站评论区 | 中低 | Cookie(SESSDATA)+Web 逆向（底座 `bilibili-api-python`，接口 `x/v2/reply/add`）；B站无官方评论 API；风控错误码 `-509`/`12015`/`12051`，小黑屋梯度封号。仅建议对自有稿件受控回复 |
| 微信（wxauto，UIA） | 中 | 官方明确不保证不被限制/封号 |
| 微信 hook / 协议 | 低 | 永久封禁风险 |
| 官方 API（微博/B站/小红书/抖音） | 低 | B站未开个人入驻、小红书需资质、抖音个人仅"小玩法" |

---

## 四、目标架构

把现有"直连"结构改造成"**统一出口 + 多渠道分发**"：

```
                    ┌──────────── 大脑层 (Brain) ────────────┐
   输入源            │  ChatPlanner  ← 发言门控（新增）        │
 ├ 桌宠气泡输入 ───→ │   ├ PromptRouter: 桌宠模式 / IM模式     │
 ├ QQ私聊/群聊  ───→ │   ├ LLMService (复用现有)               │
 ├ 定时/状态触发 ──→ │   ├ MemoryManager (五层 + 渠道隔离)     │
 └ QQ空间事件   ───→ │   └ ToolRegistry (Phase 3+)            │
                    └────────────────┬───────────────────────┘
                                     │  Utterance 统一出口（新增）
                    ┌────────────────┴───────────────────────┐
                    │  渠道适配器层 (Channel Adapters)         │
                    ├ 桌宠渠道(现有) : Bubble + Appearance + TTS
                    ├ QQ渠道(新增)   : OneBot 文字/图片/record
                    └ 浏览器渠道(Phase4): 经 dsh ACP
```

**核心洞察**：现有 `AppController::handleMakoReply` 实质上已经是一个"桌宠渠道适配器"——把它降级为"渠道之一"，新增 `QQChannelAdapter` 消费同一批 `SentenceText`，架构自然成立。

**必须解决的矛盾——双提示词模式**：
- **桌宠模式**：沿用现有 `prompt.txt`，输出 JSON（`zh_text`/`ja_text`/`tags`），驱动气泡+立绘+TTS
- **IM 模式**：新提示词，输出自然语言（+可选情绪标签）——IM 界面无处显示"中文气泡/日文语音"分身，群聊也不该用桌宠式刷屏
- 需要 `PromptRouter` 按渠道切换，**角色人格（人设、记忆、关系）两模式共用**

### 新增模块清单

| 模块 | 职责 | 技术要点 |
|---|---|---|
| `QQProtocolClient` | OneBot v11 WS 客户端（连协议端 WS 服务端） | `QWebSocket`（Qt 原生，无需 Python 中间层）；心跳/重连/`post_type=message` 事件 |
| `QQChannelAdapter` | Utterance → OneBot 消息段 | 文本 / `image`(表情包，`base64://` 或 `file:///`) / `record`(语音，wav 直接给) |
| `ChatPlanner` | **决定"该不该开口"** | 照搬 MaiBot 的 frequency/dynamic 门控（见第五节） |
| `ProactiveScheduler` | 主动发起对话 | `QTimer` + `QDateTime`，配置 schema 见第六节 |
| `SessionRouter` | 渠道会话隔离 | 记忆按 `channel + session_id` 分桶 |
| `PromptRouter` | 桌宠 JSON / IM 自然语言 | 两套提示词，共用人格与记忆 |
| `DshBridge`（Phase 4） | ACP 客户端，委派重任务 | `QProcess` + stdio JSON-RPC |

---

## 五、发言门控设计（"像人"的核心，移植自 MaiBot）

群里最难的不是"说什么"，而是"该不该说"：

| 参数 | 设计 | 来源 |
|---|---|---|
| `reply_trigger_mode = frequency` | 纯计数：累计新消息数达阈值才进 Planner | MaiBot 默认 |
| `reply_trigger_mode = dynamic` | 估计每批消息回复概率，用 **1 小时滑动窗口**把实际回复次数拉回"目标回复数 × 频率"，含**空闲退避**（仅群聊生效，私聊不设门控） | MaiBot dynamic |
| `talk_value` 话痨度 (0~1) | 越小越安静（0.3~0.5 明显话少）；支持**按时段分档** | MaiBot `talk_value_rules` |
| 连续发言上限 | 控制连续发消息时 Planner 重思考次数，防刷屏 | MaiBot |
| 群聊提示词措辞 | "读气氛、该开口才开口、不要每个人都回，优先回应主动提到你的" | MaiBot 群聊提示词 |

**关键结论**：主动消息不要"定时直接发固定文案"，而应学 MaiBot 的 `proactive.trigger`——**把 intent 写进上下文唤醒大脑，由 LLM 结合人格/记忆自行决定说什么**。

---

## 六、主动对话设计

### 触发类型

| 类型 | 条件示例 | 实现 |
|---|---|---|
| 状态驱动 | 主人空闲 > 30min、连续多轮对话后静默 | `QTimer` 检测最后交互时间 |
| 时间驱动 | 早安(7:30)、饭点(11:30/17:30)、睡前(22:30) | `QDateTime` 可配置时刻表 |
| 事件驱动 | QQ 收到新消息、QQ 空间有新互动 | 信号驱动，无需定时器 |

### 配置 schema（合并 dsh 社区已验证字段，作为可配置项实现）

```yaml
proactive:
  enabled: true
  triggers:
    - type: idle                  # 状态驱动
      min_idle_seconds: 1800      # 主人空闲 30 分钟
    - type: schedule              # 时间驱动
      at: ["07:30", "11:30", "22:30"]
      every_seconds: 300          # 周期碎碎念
  jitter_seconds: 120             # 抖动，避免机械感
  quiet_hours: ["23:00", "08:00"] # 免打扰（夜间绝不主动）
  daily_limit: 3                  # 每日主动上限
  cooldown_seconds: 900           # 开口冷却（≥15分钟）
  talk_value: 0.4                 # 话痨度 0~1（越小越安静）
  talk_value_rules:               # 按时段分档（参考 MaiBot）
    - range: "00:00-08:59"
      value: 0.8
```

### 节流（必须有）

- 每日主动次数上限、同一会话冷却时间
- 安静时段（如 23:00–08:00 不主动）
- 冷却期内即使满足条件也不触发

### 6.4 跨渠道观察与主动转述（Phase 2 设计）

> 状态：设计已确认，未实现。当前 QQ 只做白名单内的被动回复；未通过白名单的消息不记录、不回复（见 10.5.6）。

**三种权限**（互斥，按来源配置）：

| 权限 | 记录 `raw_events` | 回复 | 用途 |
|---|---|---|---|
| `reply` | ✅ | ✅ | 当前白名单群/好友 |
| `observe` | ✅ | ❌ | 只看不说，供主动转述 |
| `drop` | ❌ | ❌ | 未通过白名单 |

观察范围必须显式配置：

```yaml
qq:
  reply:
    private: allowlist
    group: mention
    private_allow: [10001, 10002]   # 示例
    group_allow: [90001]           # 示例
  observe:
    groups: [90002]       # 只记录、不回复（示例）
    private: []           # 非主人私聊默认不观察
```

**转述隐私矩阵**：

| 来源 | 默认能否告诉主人 | 内容 |
|---|---|---|
| 白名单群的公开话题 | ✅ 可聚合/匿名 | “今天群里在聊 X” |
| 群里有人提到主人/问主人在不在 | ✅ 优先 | “有人在群里问你去哪了” |
| 群里敏感/盲区内容 | ❌ | 不记录转述 |
| 非主人私聊 | ❌ 默认不转述 | 可配置“仅 metadata”：只说 U 找过你，不说内容 |
| 主人自己也在的群 | ❌ 不转述 | 主人已经知道 |

### 6.5 价值判断与投递

**Salience 评分**（先用规则，后续可加轻量 LLM 分类）：

| 信号 | 权重 |
|---|---|
| 涉及主人（@、称呼、问“主人在吗”） | 高 |
| 主人可能感兴趣的领域/话题 | 中高 |
| 强情绪、冲突、表白、重大计划 | 中高 |
| 多人重复讨论 | 中 |
| 日常闲聊/表情包 | 低 |
| 敏感/隐私/他人私事 | 直接排除 |

**投递优先级**：

1. **pet**：主人在电脑前，气泡 + TTS，最自然
2. **主人 QQ 私聊**：pet 不在/最小化，且配置允许
3. **不投递**：主人在忙/免打扰，进队列等下次

同一件事只在一个渠道说；已转述事件写回 `raw_events.id` 标记，避免重复。

### 6.6 分期与验收

| 阶段 | 范围 | 验收 |
|---|---|---|
| **2a 主人相关事件转述** | 只观察白名单群；只转述涉及主人/被问在不在；只投 pet；每日 1~3 次 | 群友闲聊不打扰；有人问主人时能转述；私聊内容永不出现 |
| **2b 群话题摘要** | 主人离开 >2 小时，回来给一次聚合摘要；匿名、不引用原话 | 不刷屏、不泄露成员身份 |
| **2c 私聊 metadata** | 默认关闭；只告诉“U 找过你”，不说内容 | 主人开启后才生效 |
| **2d 反馈学习** | 主人回复/追问提高权重；说“别说了”/长期忽略降低 | 同类事件打扰率下降 |

> 依赖：请求上下文隔离（并发不串话）→ scope/audience 模型 → 群聊上下文预算 → 2a。

---

## 七、QQ 接入详细设计

**部署**：SnowLuma v2.2.0（Windows 完整包）+ **专用小号**；SnowLuma 提供 OneBot v11 **WebSocket 服务端**（`127.0.0.1:3001`），Qt 端 `QQProtocolClient` 作为客户端连接。

**当前已实现**：私聊/群聊文本收发、`get_login_info` 自检、断线重连、白名单过滤、群聊 @ 门控、`raw_events` 落库。语音/图片/表情包与主动观察待后续批次。

| 能力 | 接口 | 说明 |
|---|---|---|
| 收私聊/群聊 | WS 事件推送 `post_type=message` | 含 `sender`(nickname/card/role)、`message` 段数组 |
| 发文字 | `send_private_msg` / `send_group_msg` | 消息段数组（推荐） |
| 表情包 | `image` 段，`file` 支持 `base64://` / `file:///` / `http(s)://` | 本地表情包库 → base64 或 file 路径 |
| 语音 | `record` 段，`file` 给 wav/mp3 | **协议端自动转 silk v3**，C++ 端无需自备编码器 |
| 收图片/表情/语音 | text/face/image/record 收发均支持 | 另有 `get_image`/`get_record`/`fetch_custom_face` |
| 高级互动 | `send_forward_msg`(合并转发)、`send_poke`(戳一戳)、`set_msg_emoji_like`(表情回应) | 拟人化加分项 |
| 空间说说 | NapCat `send_qzone_msg`；SnowLuma 另有 `comment_qzone`/`like_qzone` | 评论/点赞需 SnowLuma（Phase 3 选型点） |

**风控措施**：发送频率限制 + 随机延时 + 拟人化打字延迟 + 避免同文案批量群发 + 稳定 IP/设备 + 绑定手机养号。

---

## 八、记忆系统设计 v2（本项目的护城河）

### 8.0 已确认决策

| 决策项 | 结论 |
|---|---|
| 跨渠道记忆（目标） | **主人 owner 内互通**（pet + 主人私聊）；非主人按 `scope/audience` 隔离；群聊永不看私聊 |
| 跨渠道记忆（现状） | 短历史/摘要/情景按 `channel + session_id` 隔离；档案/关系按 `subject` 共享；`audience` 模型尚未实现 |
| 群消息存储 | **原始层全量落库 + 空闲蒸馏**（v2 修订，取代早期"只存参与"方案） |
| 关系状态 | **全对象分层**（对主人/每个好友/每个群分别维护） |
| 身份归属权 | **宿主配置层声明，模型无权判定**（mem0 模式，防 prompt 注入冒认） |
| 关系共享（B3） | 同一 subject 跨渠道**共享同一份数值**——关系是人对人的，不是人对渠道的 |
| 立绘跟随（D1） | **可配置项** `[appearance] followExternalMood`（默认开）：QQ 聊天情绪是否驱动桌设立绘 |
| 情绪/退火分层（D2） | **情绪状态全局、退火表现桌宠渠道私有**——QQ 渠道永不直接调 TimeManager |
| 群聊私密过滤（C3） | 初期简单规则：群聊 prompt 不注入 relationship 数值与亲密档案 key |

### 8.0.1 现状差距（2026-10-27）

| 问题 | 现状 | 影响 |
|---|---|---|
| 主人 QQ 私聊 vs pet | 短历史/摘要/情景不共享；档案/关系共享 | pet 不记得主人刚在 QQ 私聊里说过的事 |
| 非主人档案 | 提取会写 `subject=qq:<uin>`，但 `LLMService` 固定取 `getCharacterProfiles("user")` | 非主人档案写了但从不注入 |
| 群聊记忆 vs 私聊 | 情景按 `origin + session` 过滤，群记忆在私聊不可见 | “私聊记得群聊”不成立 |
| 盲区/敏感 | `isBlindZone()` 无调用点；`meta.sensitive` 恒 false；prompt 未过滤 sensitive | 盲区配置不生效 |
| 摘要/情景 | 同一批轮次同时产出 working_summary 与 episodic；`memory_cursor` 未使用 | 内容重叠、游标重复提取 |
| L0 消费 | `raw_events.processed` 始终为 0 | 只存不蒸馏，群聊未 @ 消息成为死数据 |
| 并发 | `m_lastMeta`/`m_lastUserInput` 全局单份 | 两条 QQ 消息并发时可能串话 |

### 8.1 dsh 社区调研结论（Memory 分类 211 个插件）

头部插件实现高度收敛为六大模式：

| 模式 | 代表插件 | 做法 |
|---|---|---|
| 双通道 | mem0、OpenViking、memory-eternal | `自动召回`（模型请求前注入）+ `自动捕获`（只捕获已完成轮次）+ 可选显式 search/add 工具 |
| 身份归属在宿主配置层 | mem0（`userId` 必填 + `allowUserOverride: false`）、workspace-memory | 记忆归属主体由配置声明，模型无权自选归属——防止 prompt 注入让 AI 冒认他人记忆 |
| 分层蒸馏 | dsh-prime-memory（L0-L3）、StrataGate（六层时间衰减） | 近期原始消息 → 远期摘要固化，注入上下文的永远是蒸馏产物 |
| 空闲整合（做梦） | mneme（autoDream）、meow-memory（idle-window + 高峰抑制） | 后台批量整理未处理消息，冲突冻结待人审 |
| fail-open | mem0 | 记忆系统故障不阻塞主对话 |
| 多层 scope | meow-memory（soul/user/project/fact/lesson/rules/topic 七层 SQLite） | soul 层（人格）全局唯一，user/project 层分作用域 |

**关键启示**（meow-memory 七层 scope）：**"渠道隔离"与"主体归属"是两个正交维度**——soul（人格）全局唯一，user（对话对象）按主体分，session（会话）按渠道分。早期方案把两者混谈是 v1 的主要缺陷。

### 8.2 身份层（新增，主人识别）

```yaml
# app_data/config/identity.yaml
self:                       # 她自己的账号（防自触发、群聊回声排除）
  qq: ["1000xxx"]
master:                     # 主人：多平台 × 多账号，全部归一为 subject='master'
  qq: ["123456789", "888xxx"]     # 主人 QQ 大号/小号
  bilibili: ["uid_xxx"]           # 未来扩展
whitelist:                  # 可选：熟人列表，优先给更高初始亲密度
  - platform: qq
    uin: "666xxx"
    note: "欧尼酱的朋友阿伟"
```

运行时解析（纯代码逻辑，不经模型）：

```
消息到达 → sender.platform + sender.uin
  ├─ 命中 self.qq        → 丢弃（自己发的）
  ├─ 命中 master.*       → subject = "master"
  ├─ 命中 whitelist      → subject = "qq:<uin>"，初始亲密度加成
  └─ 未知                → subject = "qq:<uin>"，首次建档
```

关键点：
- **按 uin 而非昵称绑定**——群里改名片/换昵称不影响识别
- 同平台多账号、跨平台身份归一到同一 subject
- 提取 prompt 只携带"渠道 + 说话人元数据"，模型只负责"记什么"，不负责"记在谁名下"

### 8.3 四层记忆全景

```
                    ┌─ soul 层（全局唯一）：character_profile + prompt 人设 ← 我是谁
   身份层            │
 identity.yaml ────→ ├─ user 层（subject 分主体）：relationship + 群友档案 ← 我和谁
 (配置声明归属)       │
                    ├─ session 层（channel+session_id 分桶）：chat_history 短期历史 ← 刚聊了什么
                    │
                    └─ raw 层（群聊原始消息）：独立表 + 保留期 + 空闲蒸馏 ← 发生过什么
```

四层各司其职：soul 全局、user 按人、session 按渠道、raw 只进不出（蒸馏后进入上层）。现有五层记忆表全部保留复用，新增的只有 `raw_messages` 表 + `identity.yaml` + 几个 ALTER 列——无向量库、无 RAG、无新框架。

### 8.4 群聊记忆：原始层 + 蒸馏层

```
群消息全量 → raw_messages 表（新，独立于 chat_history）
              │  字段: 群号/说话人/内容/时间/sensitive/consolidated标记
              │  用途: ① 门控统计 ② 她主动"回想"时按需检索 ③ 蒸馏原料
              │  保留期: 可配置（如 14 天），蒸馏完成后可清理
              │  ⚠ raw 层永不自动进 LLM 上下文——上下文干净的关键
              │
              ├─ 参与对话（被@/被回复/门控决定开口）→ chat_history（短期上下文，现流程）
              │
              └─ 空闲时段批量蒸馏（参考 autoDream/idle-window）
                   每 N 条未处理 raw 消息 → 一次低 reasoning 提取
                   → 产出 episodic/summary/relationship（带 subject）
                   → 冲突时冻结待人审（参考 mneme，复用现有记忆管理表格界面）
                   → 标记 consolidated
```

- **她能回忆起任何闲聊**（raw 层检索），但日常上下文只含蒸馏产物——上下文干净与拟人不再互斥
- 群聊记忆提取从"逐轮触发"改为"空闲批量"，成本可控（高峰抑制）
- 盲区群：raw 层直接不落库（比打标过滤更彻底）
- raw 层检索先用 SQLite LIKE + 时间排序（零依赖）；向量语义召回列为 Phase 4+ 可选项
- 蒸馏产物的盲区语义："不记住"≠"不许用"——她在盲区群里说话时，其他渠道积累的长期记忆照常注入

### 8.5 表结构改动（Phase 0 一次做全）

```sql
ALTER TABLE chat_history        ADD COLUMN channel    TEXT NOT NULL DEFAULT 'pet';
ALTER TABLE chat_history        ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default';
ALTER TABLE chat_history        ADD COLUMN speaker    TEXT;                      -- 群聊说话人
ALTER TABLE chat_history        ADD COLUMN sensitive  INTEGER NOT NULL DEFAULT 0;
ALTER TABLE relationship_state  ADD COLUMN subject    TEXT NOT NULL DEFAULT 'user';    -- 'user' 即主人（E2 修正：永久保留不迁移）
ALTER TABLE long_term_summary   ADD COLUMN channel    TEXT NOT NULL DEFAULT 'pet';     -- 摘要分桶（审查修订 A1）
ALTER TABLE long_term_summary   ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default';
ALTER TABLE episodic_memory     ADD COLUMN origin     TEXT NOT NULL DEFAULT 'pet';     -- 渠道溯源（B5/E3）
```

带 DEFAULT 的 `ALTER TABLE ADD COLUMN` 对现有数据零迁移成本。subject 命名规范（**E2 修正**）：`'user'` **永久保留**为"主人"的正式 subject 名（与 character_profile 存量语义一致），新主体用 `qq:<uin>`（好友群友）/ `group:<群号>`（群整体关系），IdentityResolver 将 master 映射输出为 `'user'`。

**摘要与未摘要游标同样按 `channel + session_id` 双键分桶**——否则 QQ 轮次会搅乱桌宠的 `coveredTurnEndId`（完整性审查 A1 挖出的漏洞）。

### 8.6 分阶段落地清单

| 阶段 | 记忆侧改动 | 涉及文件 |
|---|---|---|
| **Phase 0** | 八项 ALTER（含摘要分桶双键、episodic origin）；`saveQATurn`/`getHistoryTurn`/`getUnsummarizedTurns`/`getLatestSummaries` 扩参（全带默认值）；`identity`/`memory`/`proactive` 配置解析（QSettings INI）；`raw_messages` 建表；`followExternalMood` 解析。**细则见 10.1** | memorymanager、configmanager、新 utterance.h / ichanneladapter.h / petchanneladapter / sessionrouter |
| **Phase 1** | **前置项**：TTSService 合成/播放分离（synthesize→产物 / speak→播放，播放器归桌宠渠道）；新增 PromptAssembler（渠道可见性过滤，C3）与 GlobalMoodState（全局情绪，D2）。群聊上下文组装（"【群名】张三：xxx"格式 + 临时窗口注入）；QQ 回复触发提取（prompt 注入群聊语境）；subject=`qq:<uin>` 落地；提取触发点抽象为"渠道无关的 Utterance 消费后钩子" | memorymanager、PromptAssembler、GlobalMoodState、QQChannelAdapter |
| **Phase 2** | 门控统计直接读 raw 层滑动窗口（内存 RingBuffer + raw 表计数）；"今日主动次数"持久化用 QSettings | 新 ChatPlanner |
| **Phase 3** | 空闲批量蒸馏调度（TimeManager 空闲检测 + 低 reasoning 批量提取）；冲突冻结人审接入现有记忆管理 UI；群关系（`group:<群号>`）与群友档案启用；群氛围认知复用 episodic_memories 的 `type='group_atmosphere'` | memorymanager、提取 prompt、settingswidget |

架构红线：**提取管道 fail-open**（参考 mem0）——记忆系统故障只损失记忆，绝不阻塞对话主链路。

---

## 九、dsh 集成详解（Phase 4，旁路而非主线）

### 9.0 路径选择

- **推荐路径：ACP**（`--profile acp`，stdio 长驻）——唯一支持多会话、跨进程 `session/list|resume`、`session/request_permission` 的协议，已有第三方客户端验证可行
- **备选：SDK JSON-RPC**（`--profile sdk`）——协议面最小，Qt 实现成本最低，但无会话恢复、取消只能关进程
- **反向能力**：自写 MCP stdio server（C++/Qt 可行：stdin/stdout 一行一帧 + `QJsonDocument`，严禁往 stdout 写日志），让 dsh 调用"发 QQ 消息"等私有工具
- **浏览器能力**：Playwright MCP / Chrome DevTools MCP 可直接 `npx` 挂到 `dsh-mcp-client`
- ⚠️ 三路径均不支持 agent 主动发起——主动权必须由 Qt 持有（与本方案一致）

### 9.1 dsh 运行时分层架构

dsh 是一个**四层洋葱结构**，内核是 Cordis 插件框架（"Everything is a plugin"）：

| 层 | 组成 | 作用 | 本地证据 |
|---|---|---|---|
| ① 外部客户端 | Qt 桌宠、IDE、终端脚本 | 通过标准协议驱动 dsh，本身不含任何 agent 逻辑 | 第三方 ACP 客户端 Martty（Rust）已验证此模式 |
| ② 接口层（profile） | `acp` / `sdk` / `headless` / `web` 四种暴露面 | 决定 dsh 以什么形态被访问；profile = `package.json` 声明 bundle 列表 + `cordis.patch.yml` 分层补丁（bundle patch → profile 补丁 → `$DSH_HOME` 全局补丁 → `--patch` 参数） | `~/.dsh/profiles/desktop/` 是本机现有唯一 profile |
| ③ Agent 核心 | 会话控制器 + 工具循环 | 维护上下文窗口、决定何时调用工具、处理权限、把会话持久化到磁盘 | ACP 的 `session/new\|list\|resume\|prompt\|cancel` 全部指向这里 |
| ④ 能力插件层 | mcp-client / schedule / skill / 沙箱 shell / web-search / subagent | 可插拔的能力包，**按 profile 挂载**——headless/sdk 组合默认只有 shell/fs 工具，mcp-client 需在补丁层显式声明 | `dsh-mcp-client` 工具名固定为 `mcp__<serverName>__<tool>` |
| ⑤ 底座 | Node 24.21 + Python 3.12.14 + `deepseek-account` 凭证 | 模型调用不走 API Key，而是代理已登录账号的额度（`deepseek-flash`，reasoningEffort=high） | `.credentials.yaml` 已存在登录态 |

**最重要的认知**：这张分层图里**没有人格层**——没有记忆管理器、没有情绪状态机、没有语音合成、没有"该不该说话"的门控。dsh 是通用 agent 运行时，它假设每个会话都是无状态的任务；而茉子的五层记忆和四维立绘恰恰是**有状态的人格**。这就是为什么大脑必须留在 Qt，dsh 只能当执行器。

### 9.2 协作时的职责边界

| 职责 | 自研 Qt harness | dsh |
|---|---|---|
| 人格 / 提示词 / 情绪 / 立绘 | ✅ 唯一持有者 | ❌ 无此概念 |
| 五层记忆（SQLite） | ✅ 唯一持有者 | ❌ 不共享、不落它的库 |
| 发言门控 / 主动定时器 | ✅ | ❌（其 schedule 插件在 headless/sdk 组合下不可用） |
| GPT-SoVITS 语音 | ✅ | ❌ |
| QQ 收发（OneBot） | ✅ 自研协议端 | 可经反向 MCP 间接触发 |
| 浏览器操作 / 多步工具 / 联网搜索 / 子代理 | ❌ | ✅ 挂 Playwright MCP 即得 |

### 9.3 协作时序（DshBridge 委派流程）

进程模型：Qt 用 `QProcess` 启动 `dsh --profile acp`（独立 profile，补丁层挂上 mcp-client 指向我们），此后双方在 stdio 上以**一行一个 JSON-RPC 帧**通信。完整一轮"委派任务 → 执行 → 回流"分四个阶段：

**① 正向委派**：`ChatPlanner`（门控层）判定这轮请求需要工具能力（如"帮我查一下今天的新闻"），把任务连同人格上下文交给 `DshBridge`。Bridge 调 `session/new` 建会话（可复用，一次建立多轮使用），然后发 `session/prompt`——该方法**立即返回 `messageId`**，不阻塞 Qt 主线程。

**② 工具执行**：dsh 在自己的进程里跑工具循环，`session/update` 通知持续回流（thinking / tool_call / tool_result / final）。期间它可能调用 profile 补丁层挂的 Playwright MCP 操作浏览器，也可多轮循环调用工具。

**③ 反向挂载**：最关键的双向通道——我们自写 **MCP stdio server**（Qt 进程内），暴露 `mcp__mako__send_qq_message` 等工具。dsh 反过来能驱动我们的 QQ 渠道：浏览器里查到的东西，她可以直接"用嘴"发进群里。

**④ 回流人格化**：dsh 返回的是**原始任务产出**（搜索结果、网页摘要、执行报告），不是茉子的话。Bridge 把它作为素材送回 `LLMService` 过一遍人格化 prompt（沿用现有 JSON 协议 + 立绘标签 + GPT-SoVITS），生成带情绪的 `Utterance`，再分发到气泡/QQ 渠道。

### 9.4 安全边界设计

| 设计点 | 说明 |
|---|---|
| **记忆零共享** | dsh 拿到的只有"任务描述 + 必要上下文片段"，五层记忆库永远不出 Qt 进程——人格不会被 agent 的通用上下文污染 |
| **崩溃隔离** | dsh 是独立 `QProcess`，它崩溃/挂起只损失一次委派任务，桌宠本体和 QQ 在线状态不受影响；`session/resume` 支持 Qt 重启后接管旧会话 |
| **版本漂移防御** | dsh 是 0.2.0-rc.2 nightly，ACP 协议层可能有变化——所有协议细节封在 `DshBridge` 单个适配类里，坏了只改一处；委派失败时 `ChatPlanner` 降级为"无工具直答"，桌宠核心功能永不下线 |
| **双账本** | 日常对话走我们自己的 DeepSeek API 计费；只有委派任务消耗 dsh 的账号额度，成本天然隔离、可统计 |

一句话总结：**dsh 的架构价值 = 一个可外挂的"手"（工具循环 + MCP 生态），而我们的 harness 是不可替换的"人格"**。两者通过 ACP 这根标准化的神经接口连接——手可以换（甚至 Phase 4 不满意可以拆掉），人格不能。

---

## 十、分阶段路线图

| 阶段 | 内容 | 外部依赖 |
|---|---|---|
| **Phase 0** | 架构解耦（细则见 10.1）：`Utterance`/`RequestMeta` 统一协议 + `IChannelAdapter` 抽象 + `SessionRouter` 输入路由；记忆侧八项 ALTER + 读写扩参；identity/memory/proactive 配置解析 | **无**（零风险纯重构） |
| **Phase 0.5** | 记忆预留收缩版（细则见 10.2）：v3 迁移仅加 `episodic_memory.visibility` 列 + `appendRawMessage`（不接线）；Percept/ChannelCapability/ToolRegistry 三空壳**缓建** | **无** |
| **Phase 1** | SnowLuma 部署（小号）+ `QQProtocolClient` 收私聊/群聊 + 发文字/表情包/语音；届时完成 Percept 泛化与 raw_messages 接线。**前置项已全部完成**：identity / PromptAssembler / v4 提取链路 / TTSService 合成播放分离 / GlobalMoodState（见 10.5） | SnowLuma |
| **Phase 2** | `ChatPlanner` 发言门控（frequency/dynamic）+ `ProactiveScheduler` 按配置主动开口 | 无 |
| **Phase 3** | QQ 空间说说 + 高级互动（合并转发/戳一戳/表情回应）——**SnowLuma 内置空间互动（评论/点赞），无需换端** | SnowLuma |
| **Phase 4** | 浏览器类经 dsh ACP（Playwright MCP）；评估 dsh 混合深度 | dsh（nightly） |

**Phase 0 必须先行**：唯一"零外部依赖、零风险"的一步，且是后续所有阶段的地基。即使 QQ 方案失败，桌宠本身也变得更健壮。

### 10.1 Phase 0 详细设计（2026-10-19 完整性审查定稿）

**三条红线**：外部行为零变化 / 旧数据零迁移 / 每步独立可构建。

**关键事实**（代码核实）：表现层已天然解耦——气泡+立绘由 `TTSService::playAudioAction` 逐句回调驱动（`AppController::onPlayAudioAction`），`handleMakoReply` 只含记忆+TTS入队+提取。因此渠道适配器是薄包装，风险低于最初评估。

**新增文件**：

| 文件 | 内容 |
|---|---|
| `utterance.h` | `Utterance`（text/ttsText/tags/channel/sessionId/speaker）+ `RequestMeta`（channel/sessionId/speaker/subject）纯数据头文件 |
| `ichanneladapter.h` | `channelName()` + `sendUtterance(const Utterance&)` 纯虚接口 |
| `petchanneladapter.h/cpp` | 薄包装：订阅 `playAudioAction` → 气泡+立绘；错误反馈沿用现有槽 |
| `sessionrouter.h/cpp` | `submitInput(channel, sessionId, speaker, text)` → 身份解析 → 查历史 → `requestReply` 信号；身份由 `IdentityResolver(platform, senderId)` 决定（C2），渠道无权定归属 |

**职责切分**：`saveQATurn`/`extractMemoryAsync` 触发留 AppController（大脑侧）；气泡/语音播放进 PetChannelAdapter；**情绪/立绘联动是全局层**（applyTags 留全局，不进任何渠道——D1 开关 Phase 1 于此处生效）；TimeManager 归属标注为桌宠渠道私有（D2），Phase 0 代码零搬动。

**MemoryManager 扩展**：`ensureColumn`（PRAGMA table_info 预检 + busy_timeout=3000 + duplicate column 错误容忍，三重保险应对重复/并发启动）；八项 ALTER（见 8.5）；`raw_messages` 建表（Phase 0 只建不写）；`saveQATurn(..., meta=RequestMeta())`、`getHistoryTurn(N, channel, sessionId)`、`getUnsummarizedTurns`/`getLatestSummaries` 双键扩参——全部带默认值，旧调用点零改动。

**ConfigManager 扩展**（QSettings INI，修正早期 yaml 设想）：`[identity]`（self/master/whitelist）、`[memory]`（blindZones）、`[proactive]`（含 targets，默认 [pet/default]）；Phase 0 只做解析与只读访问器，无调度行为、无 UI 勾选框。

**askDeepSeek 扩参**：第三参 `const RequestMeta &meta = RequestMeta()`，现有调用点零改动；meta 用于提取 prompt 元数据与 subject 归属。

**实施顺序（5 提交）**：1 表结构 → 2 读写扩参 → 3 utterance.h+adapter → 4 SessionRouter+装配 → 5 配置解析。依赖：1→2→4，3、5 独立可插，无依赖倒置。

**验收清单**：全功能回归与改前一致；旧行自动 `channel='pet'`；二次启动不报 duplicate column；ini 手填主人 QQ 后 `getMasterAccounts()` 返回正确；QQChannelAdapter 占位编译通过。

**完整性审查结论**（A-E 共 14 项，2026-10-19）：挖出并修正 2 处方案漏洞——① 未摘要游标与摘要在历史分桶后失配（→ long_term_summary 双键，A1）；② 群聊"只存参与"导致旁听失忆（→ 8.4 原始层+蒸馏层）。其余结论已并入 8.0 决策表与各节正文：subject=`'user'` 永久保留不迁移（E2）；群聊防串群三重隔离 = session_id 过滤 + episodic origin 过滤 + subject 按人（E3）；TTSService 分离列为 Phase 1 前置项（E1）。

---

### 10.2 Phase 0.5 收缩裁决与架构演进预留（2026-10-20 定稿）

> **2026-10-27 更新**：Phase 0.5 已并入 10.4 的 v3 迁移，范围扩大为「visibility + raw_events 泛化 + 轮次语义 + memory_cursor」；**已实施**（提交 `8f24ae5`，真实库 v2→v3 验证通过）。本节保留为决策来源。

> 来源：与另一 agent 的探讨汇总经核实后的裁决。事实纠正：配置体系为 **identity.json（JSON）** 而非 QSettings/INI；relationship_state 为重建表而非加列；B3/C3/D1 在 Phase 0 已拍板（D1=可配置 `followExternalMood`）。

**Phase 0.5 收缩裁决**（原一揽子预留 → 收缩）：

| 项 | 裁决 | 理由 |
|---|---|---|
| `episodic_memory` + `visibility` 列 | ✅ 现在做 | Phase 1 C3 必需，schema 稳定，存量默认 `'private'`（桌宠私密经历不外泄） |
| `getActiveEpisodicMemories` 扩参（`visibility` + `origin`） | ✅ 现在做 | 默认不过滤，零风险；`origin` 供 C3 防串群三重隔离，见 10.3 |
| `appendRawMessage()` | ⚠️ 半做 | 方法现在写；**接线只限 IM 渠道**（Phase 1），桌宠已由 saveQATurn 全量入库不双写 |
| `daily_journal`/`reflection_log` 建表 | ❌ 缓到 Phase 2 | schema 未经实现检验，不提前固化 |
| Percept / ChannelCapability / ToolRegistry 三空壳 | ❌ 缓建 | 判断标准：空壳只对"schema 已定死、马上接数据"的东西有价值（raw_messages 合格）；对"形状未知、调用方不存在"的东西是仪式性代码。Percept 届时（Phase 1）改 `submitInput` 仅 1 处调用方，一行成本 |

**十问裁决记录**：#4 AgentLoop 叠加不替换（Phase 3/4 事项）；#5 daily_journal 只注入 pet+私聊；#6 B站/PC 经历默认 private；#7 代谢跟随夜间时段；#8 Percept Phase 1 再泛化；#9 位掩码之争 Phase 1 有第二个使用者时再定；#10 ToolRegistry 待 Phase 3 表情包 / Phase 4 ACP 有真实工具时建。

**架构演进预留地图**（进文档不进代码）：

1. **渠道 ≠ 工具**，未来接入物分三类——消息渠道（QQ/微信，双向有回复）、感知事件（B站观看/PC 操作，单向流入）、实时流（电话/语音通话，持续双向音频）
2. **Episode 改造三步**（核心问题："她没回的消息永远不入记忆"）——Step 1 Phase 1：QQ 渠道写 `raw_messages`；Step 2 Phase 1：反思游标从 `chat_history` 挪到 `raw_messages`（参考 dsh-qqchat 双游标：群反思 + 成员批量反思）；Step 3 Phase 2：`HistoryTurn` 增加 `triggerMessageIds`/`turnKind`。桌宠反思继续走 chat_history（每次输入必有回复，无损失）
3. **记忆代谢**（Phase 2）：`MemoryMetabolism` 每日跑（跟随夜间时段）——汇总各 origin episodic → `daily_journal` → 衰减巩固（现 `decayEpisodicMemory` 启动跑一次改为每日批处理）→ `reflection_log`
4. **主动唤醒模式**（Phase 2）：采纳 dsh-proactive 的 wake framing notice + `proactive_reclaim` 静默收尾（整轮折叠为墓碑）；openher 的 Drive 随时间演化超阈值触发。**克制项**：不采纳 Mai_love 的"独立日程生活/想念机制"重人格模拟，等门控数据跑出基线后再评估；不抄 DSH Cordis 插件内核



### 10.3 Phase 0.5 增补决议（2026-10-26）

`getActiveEpisodicMemories` 的扩参同时纳入 **`origin` 与 `visibility`** 两个过滤维度：C3 群聊「防串群」三重隔离明确要用 `origin` 过滤，提前在 Phase 0.5 一次加齐，避免 Phase 1 二次修改签名。两者默认均不过滤，旧调用零改动；`visibility` 默认 `private`（桌宠私密经历不外泄）。

### 10.4 记忆库 v3 设计决议（2026-10-27 定稿）

> **状态**：✅ 已实施（提交 `8f24ae5`；单测 15 例通过；真实库 107→108 行迁移成功）
> 触发：评估「一问一答」存储能否承载 QQ 群聊、主动对话、B 站浏览、窥屏。结论：不能。
> 范围：本决议吸收原 Phase 0.5（`visibility` / `appendRawMessage` / `origin` 过滤），统一为一次 v3 迁移。

#### 10.4.1 四项决策

| # | 决策项 | 结论 |
|---|---|---|
| 1 | 主动轮次 | `user_input` 允许 NULL，配 `turn_kind='proactive'`；**不塞空串** |
| 2 | 感知事件存储 | 统一 `raw_events`（`platform`/`event_type` 区分）；窥屏只存**摘要级事件**，原始帧不入库（将来如需另建 `percept_samples` 短保留表） |
| 3 | 可见性 | 三档 `private / channel / public` 够用；与 `sensitive` **正交** |
| 4 | 蒸馏游标 | 独立 `memory_cursor` 表，**不塞进** `long_term_summary` |

#### 10.4.2 轮次语义与渲染规则

| `turn_kind` | 含义 | `user_input` | 短期上下文注入 | 记忆提取 prompt |
|---|---|---|---|---|
| `reply` | 用户说→她回（现状） | 文本 | `user` + `assistant`（照旧） | 「用户: … / AI: …」（照旧） |
| `proactive` | 她主动开口（定时/空闲触发） | **NULL** | **只注入 `assistant`**，不生成空 `user`；历史开头若为 assistant，靠既有 system note 说明 | 「茉子（主动）: …」，不生成「用户:」行，不提取用户档案 |
| `perception` | 看到某事而开口（B 站/窥屏） | **NULL** | 同 proactive，可附「因观察到某事而开口」 | 标注触发来源，不计为用户发言 |

**规则**：
- 短期记忆 N 轮按 `turn_kind` 全部计数，主动轮占槽位（可接受；若嫌吵，后续可加「只统计 reply」配置）。
- 主动轮仍参与 `relationship_updates` / 摘要（她主动关心也影响关系），但**不产生用户档案更新**。
- `saveQATurn` 写入时空 `user_input` 绑定为 SQL NULL。

#### 10.4.3 目标 schema（v3）

**1. `chat_history`（重建：`user_input` 放开 NOT NULL，新增轮次字段）**
```sql
CREATE TABLE chat_history (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  timestamp DATETIME DEFAULT (datetime('now','localtime')),
  user_input TEXT,                                  -- 可空：proactive / perception
  raw_reply TEXT NOT NULL,
  channel TEXT NOT NULL DEFAULT 'pet',
  session_id TEXT NOT NULL DEFAULT 'default',
  speaker TEXT,
  sensitive INTEGER NOT NULL DEFAULT 0,
  turn_kind TEXT NOT NULL DEFAULT 'reply',          -- reply / proactive / perception
  trigger_event_ids TEXT,                           -- JSON 数组，指向 raw_events.id
  subject TEXT NOT NULL DEFAULT 'user'              -- user / qq:<uin> / group:<群号>
);
CREATE INDEX idx_history_session ON chat_history(channel, session_id);
```

**2. `raw_messages` → `raw_events`（泛化：全量事件流水，不进上下文）**
```sql
CREATE TABLE raw_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  platform TEXT NOT NULL,                    -- qq / bilibili / screen / system
  channel TEXT NOT NULL,                     -- pet / qq_private / qq_group / bilibili / screen
  session_id TEXT NOT NULL,
  message_id TEXT,                           -- 平台消息 ID；UNIQUE(channel, message_id) 去重
  direction TEXT NOT NULL DEFAULT 'in',      -- in / out / event
  sender_id TEXT,                            -- 稳定身份（uin/uid；自己为 'self'）
  sender_name TEXT,                          -- 显示名快照
  event_type TEXT NOT NULL,                  -- message / at / notice / percept / trigger / outbound
  content TEXT,                              -- 纯文本（可空）
  payload TEXT,                              -- JSON 原文（图片/语音/视频标题/停留时长等）
  occurred_at DATETIME DEFAULT (datetime('now','localtime')),
  sensitive INTEGER NOT NULL DEFAULT 0,
  visibility TEXT NOT NULL DEFAULT 'private',
  processed INTEGER NOT NULL DEFAULT 0,       -- 是否已被蒸馏
  UNIQUE(channel, message_id)
);
CREATE INDEX idx_raw_events_session ON raw_events(channel, session_id, occurred_at);
CREATE INDEX idx_raw_events_processed ON raw_events(processed);
```

**3. `episodic_memory`（扩两列）**
```sql
ALTER TABLE episodic_memory ADD COLUMN visibility TEXT NOT NULL DEFAULT 'private';
ALTER TABLE episodic_memory ADD COLUMN subject    TEXT NOT NULL DEFAULT 'user';
```

**4. `memory_cursor`（新增：每 channel+session 一条）**
```sql
CREATE TABLE memory_cursor (
  channel TEXT NOT NULL,
  session_id TEXT NOT NULL,
  last_event_id INTEGER NOT NULL DEFAULT 0,
  last_turn_id  INTEGER NOT NULL DEFAULT 0,
  updated_at DATETIME DEFAULT (datetime('now','localtime')),
  PRIMARY KEY(channel, session_id)
);
```

**5. 不动的三张表**：`character_profile`（已按 subject）、`relationship_state`（已按 subject）、`long_term_summary`（已按 channel+session 分桶）。

#### 10.4.4 `visibility` 语义

| 值 | 可见范围 | 典型内容 | 默认来源 |
|---|---|---|---|
| `private` | 仅桌宠/主人，**不进任何外部渠道上下文** | 窥屏、B 站私密浏览、桌宠私密对话 | 桌宠、B 站、窥屏 |
| `channel` | 仅同一渠道会话（同一 QQ 群 / 同一好友 / 同一 B 站会话） | 群聊记忆、群友档案、群内约定 | QQ 群 / 私聊 |
| `public` | 可跨渠道复用的人格/公开事实 | 公开人设、自我介绍、主人明确允许共享的信息 | **仅手动/高置信写入** |

约束：`public` 不自动升级（episodic 默认 private/channel）；`sensitive` 与 `visibility` 同时保留，前者管「不要主动提/不进长期」，后者管「谁能看到」。

#### 10.4.5 v2 → v3 迁移

1. **重建 `chat_history`**：建 v3 表 → 复制存量（`turn_kind='reply'`、`subject='user'`、`trigger_event_ids=NULL`）→ 删旧表 → 重命名 → 重建索引。
2. **`raw_messages` → `raw_events`**：建新表 → 复制存量（`platform`：qq*→qq/其余→pet；`direction='in'`；`event_type='message'`；`sender_name=speaker`；`occurred_at=timestamp`；`visibility='private'`；`processed=consolidated`）→ 删旧表。
3. `episodic_memory` 两个 ADD COLUMN（带默认值，存量零迁移）。
4. 新建 `memory_cursor`。
5. `PRAGMA user_version = 3`；全程事务，失败整体回滚。

#### 10.4.6 验收清单

- [x] v1/v2 库升级到 v3，存量数据零丢失，二次启动不重复迁移（`legacyMigrationPreservesData` / `migratesFromV2PreservingData` / `migrationIsIdempotent`）。
- [x] 新建库直接为 v3，7 张表结构正确（`freshDatabaseIsV3`）。
- [x] `reply` 轮行为与改前一致；`proactive` 行 `user_input IS NULL`（`proactiveTurnAllowsNullInput`）。短期注入与提取 prompt 的渲染规则已实现，真实主动轮次待 Phase 1 实跑验证。
- [x] `raw_events` 可写入、`message_id` 去重生效、默认不参与上下文（`appendRawEventDedupAndFields`）。
- [x] `episodic_memory` 按 `origin`/`visibility` 过滤，默认不过滤（`episodicOriginAndVisibilityFilters`）。
- [x] `memory_cursor` upsert/读取正确（`memoryCursorUpsert`）。
- [x] 全量 ctest 通过（3 套件 / 31 例）。

#### 10.4.7 本阶段明确不做

- 不接 QQ/B 站/窥屏数据源；不实现屏幕原始帧/OCR 存储。
- 不改蒸馏逻辑本身（Phase 1 再让提取管道读 `raw_events` + 推进 `memory_cursor`）。
- 不做 `chat_history` 的 UI 类型展示（设置界面历史表格 Phase 1 再适配）。

> **2026-10-27 追加（v4）**：在 v3 基础上给 `episodic_memory` 增加 `session_id`（C3 跨群隔离），并把渠道/会话/说话人/主体带进提取链路；详见 10.5。

### 10.5 Phase 1 前置实施记录（2026-10-27）

> 代码提交：`1aa4436`（identity + PromptAssembler + v4 提取链路）；当时测试：7 套件 / 64 例全绿；真实库 v3→v4 迁移验证通过。
>
> **2026-10-31 更新**：记忆 v5 R3 主体（PR-1~3.2c）与 PR-4 MemoryAgent 已合入，并补充异常昵称清洗、时间范围检索、partial timeout；当前基线为 **15 套件 / 145 个测试函数**、数据库 `user_version=9`（14 张表）。3.2a/3.2a.1 已手工验证，3.2b/3.2c 待手工验收；PR-5 未开始。上面 7/64 为 2026-10-27 时点记录，保留不改。

#### 10.5.1 identity 身份接入

- `SessionRouter::submitInput(channel, sessionId, speaker, text, senderId="")`：增加稳定身份参数，返回是否进入大脑
- `resolveSubject()`：桌宠/空 senderId→`user`；QQ `self`→丢弃；`master`→`user`；未知→`qq:<uin>`；其他平台→`<channel>:<id>`
- `ConfigManager::loadIdentityConfig()` / `reloadIdentity()`；`SessionRouter` 构造时显式加载，配置错误在启动日志暴露
- 来源：`identity.json` 的 `identity.self / identity.master`（按 uin 绑定，与昵称无关）

#### 10.5.2 PromptAssembler 上下文组装

- 新增 `PromptAssembler`（纯格式化 + 过滤，不持有数据库）
- `buildSystemPrompt()`：基础 prompt + 茉子人设 + 主人档案 + 情景记忆 + 摘要 + 关系状态
  - 群聊：不注入关系数值；过滤含「关系/称呼/情感/依赖/性格」的主人档案 key
  - 情景记忆：`private` 仅桌宠、`channel` 需 `origin + session_id` 匹配、`public` 全渠道；桌宠按主人特权可见全部
- `buildHistoryMessages()`：短期记忆 messages；`proactive/perception` 只注入 assistant；群聊用户消息加说话人前缀（`小明：…`）

#### 10.5.3 v4 提取链路（记忆库 v4）

- `episodic_memory` 增加 `session_id TEXT NOT NULL DEFAULT 'default'` + `idx_episodic_origin_session`
- `extractMemoryAsync(..., const RequestMeta &meta)`；提取 prompt 顶部注入「渠道/会话/说话人/主体」，群聊对话行格式 `阿伟(qq:10001): …`
- 档案准入三条：长期有效 / 本人明确陈述 / 非敏感；时间性、情绪、隐私、纠纷只进 episodic；`subject` 支持 `user/mako/qq:<uin>/group:<群号>`
- 写入链路按本轮 meta 归属：`origin=channel`、`visibility=pet?private:channel`、`subject`（模型给出，缺省 meta.subject）、`session_id=meta.sessionId`；摘要按渠道+会话分桶；关系按 meta.subject
- `memoryExtractionReady(..., const RequestMeta &meta)` 随信号回传，替代全局 `m_lastMeta`，避免异步窗口漂移

#### 10.5.4 已确认的策略

- 桌宠采用**主人特权**：可见群聊事件（sensitive 仍按提取规则不进档案）
- 关系值按 subject 跨群共享；群聊 prompt 继续隐藏数值（先不改）
- 摘要与情景记忆内容重叠的问题（summary 兼任蒸馏游标导致）**暂缓**，后续接 `memory_cursor` 时一并调整

#### 10.5.5 前置项完成情况

- [x] TTSService 合成/播放分离（批次 C）：新增 `IAudioSink` / `PetAudioSink`，播放器与播放队列归桌宠渠道；`TTSService` 只负责合成；无 sink 时只合成不播放。人工回归：多句流式、单句、起播/切换时序正常
- [x] GlobalMoodState（批次 D）：`emotion/blush/distance` 提升为全局状态；桌宠侧 `applyPetTags()` 同步立绘与全局情绪；外部渠道情绪按 `followExternalMood`（D1，来自 `identity.json [appearance]`）决定是否驱动桌宠立绘；退火仍归桌宠渠道（D2）。自动化 5 例；桌宠人工回归通过

至此 Phase 1 前置项全部完成，随后进入 **Phase 1 主体**（QQ 文本进展见 10.5.6）。

#### 10.5.6 Phase 1 主体进展（QQ 文本，2026-10-27）

- [x] SnowLuma v2.2.0 完整包 + OneBot WebSocket 服务端（`127.0.0.1:3001`）
- [x] `QQProtocolClient`：连接/重连（3s→30s）、token 鉴权、`get_login_info` 自检、动作发送
- [x] `QQMessageParser`：私聊/群聊、text/at/image/record/face、`mentionedSelf`、原始 JSON
- [x] 白名单与 @ 门控：未通过不记录/不回复；通过群未 @ 只记录；`retcode=100` 4 秒后重试一次
- [x] 入站 → `raw_events` → `SessionRouter`；QQ 文本回复 → `QQChannelAdapter`，不触发桌宠气泡/TTS
- [ ] QQ 语音/图片/表情包输出
- [ ] 群聊滑动窗口/发言门控（Phase 2）

---

## 十一、风险清单

| 风险 | 等级 | 说明 |
|---|---|---|
| **QQ 封号** | **高** | SnowLuma 注入 NTQQ，腾讯有检测；必须小号、养号、控频。**无绝对安全** |
| 人格漂移 | 中 | 多群多人并发时人格不一致，需共享人格档案 + 严格记忆隔离 |
| 隐私/社交风险 | 中→低 | 已定公开 AI 身份，群友预期明确；群聊内容入库需谨慎 |
| 合规 | 低 | 公开 AI 身份 + 仅好友/自家群，风险显著下降 |
| 商用限制 | 中 | NapCat 许可证禁商用、禁公开分发修改版；SnowLuma 为开源框架，商用风险更低 |
| dsh 依赖 | 中 | 仅 Phase 4，非核心路径；nightly rc 接口会漂移 |
| 成本 | 低-中 | dsh 走账号额度；自研走 DeepSeek API 按量计费 |

---

## 十二、待定事项

| # | 事项 | 说明 |
|---|---|---|
| 1 | ~~Phase 0 实施范围~~ | **已实施并通过验收**（提交 `61f1767`；23 例测试全绿；2026-10-26 复核构建/测试通过） |
| 2 | ~~Phase 3 协议端选型~~ | **已定 SnowLuma**（v0.5 调研修订，见 3.2）：空间评论/点赞内置，Phase 3 无需换端 |
| 3 | QQ 小号准备 | 需专用小号并养号（绑定手机、稳定设备、避免新号立刻群发） |
| 4 | SnowLuma 部署验证 | Phase 1 实施前先用小号独立验证收发链路（私聊/群聊/图片/语音） |
| 5 | IM 模式提示词 | Phase 1 前需单独设计 IM 模式系统提示词（自然语言输出 + 公开 AI 身份声明） |

---

## 附：本文档信息来源

- 本地核实：`D:\DSH` 安装目录（app.asar 包结构、profile 机制、runtime 版本）
- 外网核实（2026-10-19）：MaiBot / AstrBot / Super Agent Party / Open-LLM-VTuber 官方文档；NapCat / SnowLuma API 文档；QQ 机器人开放平台文档；dsh 社区插件仓库（dsh-proactive、dsh-heartbeat、dsh-pet、dsh-whale-musume 等）
- 记忆系统调研（2026-10-19）：[Awesome DSH Plugin Memory 分类](https://awesome-dsh-plugin.com/memory/)（211 个插件）；[Mem0 × DeepSeek Harness 集成文档](https://docs.mem0.ai/integrations/deepseek-plugin)；dsh-prime-memory（L0-L3 蒸馏）、dsh-workspace-memory（作用域隔离）、mneme（autoDream）、meow-memory（七层 scope）
- 未核实项已在正文标注；所有外部链接以调研当日状态为准
