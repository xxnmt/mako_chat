# B5 配置盘点与生效策略

依据源码盘点（`ConfigManager`、`settingspages`、`AppController`、各模块读取点）。
生效分类：**1** 已正确热更新；**2** 可热更新但未接线（现已在保存差异中接线）；
**3** 需在安全任务边界更新；**4** 需重连/重新加载局部服务；**5** 兼容/未实现/仅诊断。

## A. 文件与入口

| 文件 | 界面入口 | 读取者 | 保存机制 |
|---|---|---|---|
| `setting.json` | 模型与服务、语音合成、记忆与检索、QQ 渠道、隐私规则、高级与诊断、时间与状态 | ConfigManager → 各模块/每轮读取 | `writeConfigDocument`（QSaveFile 原子写、未知字段保留） |
| `identity.json` | 身份与盲区、认知与自主行为 | ConfigManager（启动/写入后 reloadIdentity） | 同上 |
| `prompt.txt` | 角色设定 | LLMService 每次请求 `loadSystemPrompt()` | 同样原子写 + 非空校验 |

未展示字段由保存逻辑保留；保存只写本页实际编辑的绑定字段。环境变量：
`MAKO_COGNITIVE_CONTEXT_ACTUAL`（actual 临时覆盖）、`MAKO_COGNITIVE_SHADOW`（只读对照）、
检索 embedding 密钥环境变量。

## B. 关键配置映射

| 配置路径 | 界面 | 默认/范围 | 实际读取者 | 生效时机 | 类别 |
|---|---|---|---|---|---|
| `api.deepseek_api_key` | 模型与服务 | "" | LLMService `setApiKey`/请求头 | 新请求（差异时 setApiKey） | 1 |
| `api.dashscope_api_key/base_url` | 模型与服务 | "" | 视觉/检索/索引 | 新请求/下一次索引批次 | 1/3 |
| `api.vision_enabled/vision_model` | 模型与服务 | false/qwen3-vl-plus | LLMService 每轮读 ConfigManager | 下一次请求 | 1 |
| `tts.mode` | 语音合成 | api | TTSService | 差异时 reloadProvider；provider 安全切换 | 4 |
| `tts.python_path/gpt_sovits_root_path` | 语音合成 | "" | TTSProcessManager | 同上 | 4 |
| `tts.gpt_weights/sovits_weights` | 语音合成 | "" | TTSService switchModel | 安全边界切换模型 | 3 |
| `tts.reference_audio_dir` | 语音合成 | app_data/reference_audio | TTSReferenceManager | 下一次合成 | 1 |
| `api.gpt_sovits_url` | 语音合成 | http://127.0.0.1:9880 | ApiTTSProvider | 下一次请求 | 1 |
| `conversation.input_pause_ms` | 记忆与检索 | 1600（0..6000） | ChatWidget 每轮读取 | 下一次输入 | 1 |
| `memory.short_term_length/summary_length` | 记忆与检索 | 15/15 | MemoryManager 每轮读取 | 下一次请求 | 1 |
| `memory.short_context_enabled/long_term_memory_enabled` | 记忆与检索 | true/true | 策略/检索 | 下一次请求；长期开关变化走权限 epoch | 1/2 |
| `memory.retrieval.index_enabled/auto_backfill` | 记忆与检索 | true/false | MemoryIndexWorker | 差异时 reconfigure（安全批次） | 3 |
| `memory.retrieval.candidate_limit/rerank_limit/rerank_enabled` | 记忆与检索 | 40/10/true | 检索查询 | 下一次检索（不打断索引） | 1 |
| `memory.retrieval.embedding_*`、`rerank_endpoint/model` | 高级与诊断 | 空/0 | MemoryIndexWorker/检索服务 | 差异时 reconfigure；向量模型/维度/provider 变化走安全批次，不自动全库重建 | 3/5 |
| `memory.agent_*` 主参数 | 记忆与检索 | 见 UI | LLMService/MemoryAgentPlanner 每轮读取 | 下一轮对话 | 1 |
| `memory.agent_plan_v2/agent_trigger` | 历史字段 | 保留原值 | 无运行消费者 | 无需迁移 | C2 |
| `memory.recent_group_context.*` | QQ 渠道 | 见 UI | 检索/上下文构建 | 下一次请求 | 1 |
| `memory.qq_group_idle_minutes/qq_group_batch_size` | QQ 渠道 | 15/50 | RawEventProcessor | 下一批整理 | 1 |
| `memory.interaction_*` | QQ 渠道 | 90/20/10 | InteractionSegmenter | 下一次整理 | 1 |
| `memory.owner_cross_channel_private` | 隐私规则 | false | 记忆访问策略 | 下一次请求；变化立即走权限 epoch | 1/2 |
| `memory.privacy_rules/policy_version` | 隐私规则 | r3 | 输出过滤 | 下一次请求 | 1 |
| `qq.enabled/ws_url/access_token` | QQ 渠道 | false/ws://127.0.0.1:3001/"" | QQProtocolClient | 差异时 setConfig + 重连（unknown 不重发） | 4 |
| `qq.reply.*`、`qq.group_permissions/private_permissions` | QQ 渠道 | 见 UI | QQInboundPolicyEngine | 下一条消息（不重连） | 1 |
| `qq.group_memory_enabled/group_memory_mode/group_sender_allowlist` | QQ 渠道 | false/…/… | 记忆/群成员服务 | 下一批/差异时刷新 | 1/2 |
| `qq.membership_refresh_minutes/max_age_minutes` | QQ 渠道 | 30/1440 | GroupMembershipService | 差异时 refresh/start | 1 |
| `qq.group_retention_days` | QQ 渠道 | 30 | 仅保存 | 到期清理未接入 | 5 |
| `logging.directory/separate_runs` | 高级与诊断 | ""/true | RuntimeLog | 差异时 configure | 1 |
| `memory.debug_full_logs` | 高级与诊断 | false | 日志 | 下一次日志 | 1 |
| `time.*` | 时间与状态 | 见 UI | TimeManager | 差异时 applyTimeSettings+start | 1 |
| `identity.self.qq/master.qq` | 身份与盲区 | [] | SessionRouter/MemoryManager | 差异时更新账号 + 权限 epoch | 2 |
| `memory.blind_zones` | 身份与盲区 | [] | 输入/记忆访问策略 | 下一次请求 + 权限 epoch | 2 |
| `appearance.follow_external_mood` | 身份与盲区 | true | GlobalMoodState | 差异时 setFollowExternalMood | 1 |
| `proactive.enabled/private_writing/targets/quiet_hours` | 认知与自主行为 | 见 UI | CognitiveCoordinator/权限 | 下一次调度；权限字段立即 epoch | 1/2 |
| `proactive.daily_limit` | 认知与自主行为 | 3（0..999） | 回顾领取 | 下一次调度/下一日统计 | 1 |
| `proactive.contact_daily_limit` | 认知与自主行为 | 3（0=不限） | 主动联系提交 | 下一次提交 | 1 |
| `proactive.cooldown_seconds/idle_seconds` | 认知与自主行为 | 900/1800 | 调度器 | 下一次调度；不重置既有唤醒/活动 | 1 |
| `proactive.concern_default_check_minutes/retention_days` | 认知与自主行为 | 720/14 | 关注链 | 下一轮；retention 到期清理未接入 | 1/5 |
| `proactive.schedule` | 已隐藏 | [] | 无 | 调度未接入，仅保留配置 | 5 |
| `cognition.open_wake.*` | 认知与自主行为 | 见 UI | OpenWake 调度 | 下一轮独立运行/调度边界；不重复创建既有唤醒 | 1/2 |
| `cognition.snapshot_budget_bytes` | 认知与自主行为 | 4096（2048..8192） | 快照构建 | 下一轮认知 | 1 |
| `cognition.review_backoff_seconds/max_review_failures` | 认知与自主行为（高级） | [120,300,900]/4 | 回顾退避 | 下一次失败/恢复；不清空既有退避状态 | 1 |
| `cognition.context_actual_enabled` | 认知与自主行为 | false | LLMService 每次独立运行读取 | 下一轮；env/setter 优先；同一轮不混用 | 1 |
| `prompt.txt` | 角色设定 | 内置 | LLMService 每请求 | 下一次请求 | 1 |
| `MAKO_COGNITIVE_CONTEXT_ACTUAL` | 环境变量（设置页显示状态） | 未设置 | LLMService | 每次独立运行读取；显式 0/false/off 覆盖关闭 | 1 |
| `MAKO_COGNITIVE_SHADOW` | 环境变量（诊断） | 未设置 | LLMService | 启动读取；只读对照 | 5 |

## C. 保存差异更新（AppController）

只比较保存前后有效配置快照，不新增事件总线：

- 日志 → `RuntimeLog::configure`；
- DeepSeek Key → `setApiKey`（不重建 LLMService、不清空对话）；
- DashScope/索引/向量字段 → `MemoryIndexWorker::reconfigure`（相同配置早退、
  运行中批次用旧配置完成后再按新配置继续）；
- TTS → `reloadProvider`（内部相等性 + 权重安全切换）；
- QQ 连接参数 → `setConfig` + 重连；普通回应/权限只更新配置；
- 群成员相关 → `GroupMembershipService::start`；
- 时间 → `applyTimeSettings` + `TimeManager::start`；
- 外观 → `setFollowExternalMood`；
- 权限相关（主人/盲区/跨渠道/主动/OpenWake 允许联系/长期记忆）→
  `reconcileActionPermissions` + action epoch；
- 认知/记忆普通参数 → 不触发模块重载，下一轮生效；
- 无修改保存 → 不调用任何模块更新、不重写文件。

## D. 校验与错误

- UI 范围与 ConfigManager 夹紧一致（示例：快照 2048..8192、唤醒上限 0..96、
  退避 30..86400 秒且 1..5 个、`min ≤ default ≤ max`）。
- 非法组合在写入前给出具体中文原因，不静默“已应用”。
- 保存后回读有效值；identity 认知字段显示 ConfigManager 归一后的值。
- `writeConfigDocument` 保持 QSaveFile 原子写、并发编辑冲突检测、未知字段保留。
- 读取/写入失败显示错误；ConfigManager 读取损坏时使用安全默认值并记录，不清空原文件。

## E. 仍需局部服务重载/边界处理的参数

| 参数 | 原因 |
|---|---|
| QQ 启停/地址/令牌 | WebSocket 必须重连；unknown 发送结果不自动重发 |
| TTS provider/安装目录/权重 | 进程/模型资源需重新加载或安全切换 |
| 索引/向量 provider/模型/维度/端点 | 需保持向量兼容性，在安全批次边界重建/回填 |
| actual 开关 | 同一认知运行内不得混用两种上下文模式，故下一轮生效 |
| 认知/主动/调度普通参数 | 下一轮独立运行或调度边界生效，避免打断当前工作 |

## F. 不新增/不改变

- 不新增数据库 schema；不删除原始历史；不重建整库索引（除非既有索引维护入口）。
- 不新增独立权限规则；权限仍由 MemoryAccessPolicy/reconcile/epoch/发送前复验决定。
- 不取消正常对话、不重建 LLMService、不因普通配置更新清空活动/草稿/退避/内部念头。
