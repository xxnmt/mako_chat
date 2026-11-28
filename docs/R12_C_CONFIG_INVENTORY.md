## 附录：逐项配置索引

按设置页面及组的源码顺序。默认和范围列为 UI 声明，解析列是 ConfigManager 同名键候选定位，运行列按解析成员列出引用候选。对于多个同名叶键，以 JSON 全路径和前文说明为准。未映射项不能据此判为无消费者。


### DeepSeek

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| API 密钥 / `api.deepseek_api_key` | `""` / `secret` | 689 / 通用或未单列 | 66, 343 | 见正文 |

### 阿里云百炼

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| API 密钥 / `api.dashscope_api_key` | `""` / `secret` | 692 / 通用或未单列 | 67, 344 | 见正文 |
| 服务地址 / `api.dashscope_base_url` | `""` / `text` | 693 / 通用或未单列 | 68, 345 | 见正文 |
| 识别对话图片 / `api.vision_enabled` | `false` / `bool` | 694 / 100 | 60, 346 | 见正文 |
| 视觉模型 / `api.vision_model` | `"qwen3-vl-plus"` / `text` | 695 / 101 | 61, 347 | 见正文 |

### 播放与服务

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 语音方式 / `tts.mode` | `"api"` / `choice:api=GPT-SoVITS\|mock=模拟播放` | 700 / 88 | 325, 490 | 见正文 |
| API 地址 / `api.gpt_sovits_url` | `"http://127.0.0.1:9880"` / `text` | 701 / 通用或未单列 | 69, 348 | 见正文 |
| Python 可执行文件 / `tts.python_path` | `""` / `file` | 702 / 通用或未单列 | 332, 497 | 见正文 |
| GPT-SoVITS 安装目录 / `tts.gpt_sovits_root_path` | `""` / `directory` | 703 / 通用或未单列 | 329, 494 | 见正文 |

### 参考音频

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 参考音频文件夹 / `tts.reference_audio_dir` | `""` / `reference_directory` | 706 / 89 | 326, 491 | 见正文 |

### 模型文件

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| GPT 权重 / `tts.gpt_weights_path` | `""` / `file` | 709 / 通用或未单列 | 327, 492 | 见正文 |
| SoVITS 权重 / `tts.sovits_weights_path` | `""` / `file` | 710 / 通用或未单列 | 328, 493 | 见正文 |
| GPT 模型目录 / `tts.gpt_model_dir` | `""` / `directory` | 711 / 通用或未单列 | 330, 495 | 见正文 |
| SoVITS 模型目录 / `tts.sovits_model_dir` | `""` / `directory` | 712 / 通用或未单列 | 331, 496 | 见正文 |

### 对话记忆

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 连续输入等待（毫秒） / `conversation.input_pause_ms` | `1600` / `num:0:6000` | 717 / 102 | 55, 483, 567 | 见正文 |
| 使用近期对话 / `memory.short_context_enabled` | `true` / `bool` | 718 / 通用或未单列 | 91, 353 | 见正文 |
| 近期对话轮数 / `memory.short_term_length` | `15` / `num:0:999` | 719 / 通用或未单列 | 86, 351 | 见正文 |
| 使用长期记忆 / `memory.long_term_memory_enabled` | `true` / `bool` | 720 / 通用或未单列 | 93, 354 | 见正文 |
| 摘要间隔（轮） / `memory.summary_length` | `15` / `num:1:999` | 721 / 通用或未单列 | 87, 352 | 见正文 |

### 检索和索引

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用本地检索索引 / `memory.retrieval.index_enabled` | `true` / `bool` | 724 / 51 | 160, 381 | 见正文 |
| 自动回填空索引 / `memory.retrieval.auto_backfill` | `false` / `bool` | 725 / 52 | 162, 382 | 见正文 |
| 候选数量上限 / `memory.retrieval.candidate_limit` | `40` / `num:10:200` | 726 / 53 | 165, 383 | 见正文 |
| 启用重排 / `memory.retrieval.rerank_enabled` | `true` / `bool` | 727 / 54 | 169, 385 | 见正文 |
| 重排数量上限 / `memory.retrieval.rerank_limit` | `10` / `num:1:40` | 728 / 55 | 167, 384 | 见正文 |

### 主动回忆工具

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用补查工具 / `memory.agent_enabled` | `true` / `bool` | 731 / 通用或未单列 | 129, 395 | 见正文 |
| 回忆方式 / `memory.agent_mode` | `"tool"` / `choice:tool=模型工具\|legacy=独立规划` | 732 / 64 | 131, 396 | 见正文 |
| 工具协议 / `memory.agent_protocol` | `"native"` / `choice:native=原生调用\|json_envelope=JSON 信封` | 733 / 65 | 136, 397 | src/cognition/llmservice.cpp:1691,1693,4694,4698 |
| 工具超时（毫秒） / `memory.agent_timeout_ms` | `6000` / `num:1000:15000` | 734 / 69 | 152, 402 | 见正文 |
| 最多工具轮数 / `memory.agent_max_rounds` | `2` / `num:1:4` | 735 / 68 | 154, 403 | 见正文 |
| 最多返回条数 / `memory.agent_max_results` | `8` / `num:1:20` | 736 / 70 | 156, 404 | 见正文 |

### 连接

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用 QQ 渠道 / `qq.enabled` | `false` / `bool` | 741 / 通用或未单列 | 108, 272, 362, 425, 908, 924, 952 | 见正文 |
| WebSocket 地址 / `qq.ws_url` | `"ws://127.0.0.1:3001"` / `text` | 742 / 通用或未单列 | 273, 426 | 见正文 |
| 访问令牌 / `qq.access_token` | `""` / `secret` | 743 / 通用或未单列 | 274, 427 | 见正文 |

### 回复范围

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 私聊回应范围 / `qq.reply.private` | `"master"` / `choice:none=关闭\|master=仅主人\|allowlist=允许列表\|all=全部` | 746 / 76 | 278, 430 | 见正文 |
| 群聊回应方式 / `qq.reply.group` | `"none"` / `choice:none=关闭\|mention=仅被 @ 时\|all=全部` | 747 / 75 | 279, 431 | 见正文 |
| 主人可在任意群触发 / `qq.reply.master_any_group` | `false` / `bool` | 748 / 74 | 280, 432 | 见正文 |
| 空名单也限制群回复 / `qq.reply.restrict_empty_group_list` | `false` / `bool` | 749 / 77 | 283, 443 | 见正文 |
| 群权限 / `qq.group_permissions` | `QJsonObject{}` / `group_permissions` | 750 / 78 | 专用控件或未映射 | 见正文 |
| 私聊账号权限 / `qq.private_permissions` | `QJsonObject{}` / `private_permissions` | 751 / 79 | 专用控件或未映射 | 见正文 |

### 群聊记忆

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用群聊记忆 / `qq.group_memory_enabled` | `false` / `bool` | 754 / 通用或未单列 | 288, 451 | 见正文 |
| 保留天数（清理暂未接入） / `qq.group_retention_days` | `30` / `num:1:3650` | 755 / 84 | 292, 457 | 见正文 |
| 成员刷新间隔（分钟） / `qq.membership_refresh_minutes` | `30` / `num:0:1440` | 756 / 83 | 294, 458 | 见正文 |
| 成员缓存有效期（分钟） / `qq.membership_max_age_minutes` | `1440` / `num:1:10080` | 757 / 82 | 296, 460 | 见正文 |
| 各群记忆模式 / `qq.group_memory_mode` | `QJsonObject{}` / `modes` | 758 / 80 | 298, 468, 586 | 见正文 |
| 各群发送者允许列表 / `qq.group_sender_allowlist` | `QJsonObject{}` / `senders` | 759 / 81 | 312, 479, 587 | 见正文 |

### 群消息整理

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 空闲后整理（分钟） / `memory.qq_group_idle_minutes` | `15` / `num:1:1440` | 762 / 通用或未单列 | 95, 356 | 见正文 |
| 开始整理的消息数 / `memory.qq_group_batch_size` | `50` / `num:1:500` | 763 / 通用或未单列 | 97, 357 | 见正文 |
| 分段间隔（秒） / `memory.interaction_gap_seconds` | `90` / `num:10:3600` | 764 / 85 | 99, 358 | 见正文 |
| 每段最多消息数 / `memory.interaction_max_events` | `20` / `num:1:100` | 765 / 86 | 101, 359 | 见正文 |
| 每段最长时间（分钟） / `memory.interaction_max_minutes` | `10` / `num:1:120` | 766 / 87 | 103, 360 | 见正文 |

### 近期群聊上下文

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 使用近期群聊上下文 / `memory.recent_group_context.enabled` | `true` / `bool` | 769 / 通用或未单列 | 108, 272, 362, 425, 908, 924, 952 | 见正文 |
| 回看范围（分钟） / `memory.recent_group_context.max_age_minutes` | `30` / `num:1:1440` | 770 / 通用或未单列 | 110, 363 | 见正文 |
| 每群候选上限 / `memory.recent_group_context.candidate_limit_per_group` | `50` / `num:1:500` | 771 / 通用或未单列 | 112, 364 | 见正文 |
| 最多参考群数 / `memory.recent_group_context.max_owner_groups` | `2` / `num:1:20` | 772 / 通用或未单列 | 114, 366 | 见正文 |
| 定向查找条数 / `memory.recent_group_context.targeted_item_limit` | `10` / `num:1:100` | 773 / 通用或未单列 | 116, 367 | 见正文 |
| 定向查找字数 / `memory.recent_group_context.targeted_char_budget` | `1200` / `num:100:8000` | 774 / 通用或未单列 | 118, 369 | 见正文 |
| 概览条数 / `memory.recent_group_context.overview_item_limit` | `50` / `num:1:500` | 775 / 通用或未单列 | 120, 371 | 见正文 |
| 概览字数 / `memory.recent_group_context.overview_char_budget` | `2200` / `num:100:12000` | 776 / 通用或未单列 | 122, 373 | 见正文 |
| 近期窗口条数 / `memory.recent_group_context.hot_window_items` | `10` / `num:1:100` | 777 / 通用或未单列 | 124, 375 | 见正文 |
| 较旧单条消息字数 / `memory.recent_group_context.warm_line_max_chars` | `24` / `num:8:200` | 778 / 通用或未单列 | 126, 377 | 见正文 |

### 记忆权限

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 允许主人跨私聊渠道使用个人记忆 / `memory.owner_cross_channel_private` | `false` / `bool` | 783 / 71 | 143, 398 | 见正文 |
| 策略版本标识 / `memory.privacy_policy_version` | `"r3"` / `text` | 784 / 通用或未单列 | 201, 405 | 见正文 |
| 内容过滤规则 / `memory.privacy_rules` | `QJsonArray{}` / `rules` | 785 / 72 | 206, 415, 584 | 见正文 |

### 身份与会话

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 茉子的 QQ 账号 / `identity.self.qq` | `QJsonArray{}` / `ids` | 790 / 通用或未单列 | 270, 271, 487, 556, 585, 586, 587, 885, 888 | 见正文 |
| 主人的 QQ 账号 / `identity.master.qq` | `QJsonArray{}` / `ids` | 791 / 通用或未单列 | 270, 271, 487, 556, 585, 586, 587, 885, 888 | 见正文 |
| 记忆盲区 / `memory.blind_zones` | `QJsonArray{}` / `zones` | 792 / 73 | 591, 893 | 见正文 |

### 桌宠表现

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 桌宠跟随外部渠道情绪 / `appearance.follow_external_mood` | `true` / `bool` | 795 / 通用或未单列 | 904 | 见正文 |

### 主动联系与回顾

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 允许主动联系 / `proactive.enabled` | `false` / `bool` | 800 / 通用或未单列 | 108, 272, 362, 425, 908, 924, 952 | 见正文 |
| 允许私下回顾与创作 / `proactive.private_writing` | `false` / `bool` | 801 / 90 | 909 | 见正文 |
| 关注再检查间隔（分钟） / `proactive.concern_default_check_minutes` | `720` / `num:0:10080` | 802 / 95 | 917 | 见正文 |
| 关注保留天数（到期清理暂未接入，仅保存） / `proactive.concern_retention_days` | `14` / `num:1:365` | 803 / 96 | 919 | 见正文 |
| 回顾间隔（秒） / `proactive.cooldown_seconds` | `900` / `num:60:604800` | 804 / 97 | 911 | 见正文 |
| 空闲等待（秒） / `proactive.idle_seconds` | `1800` / `num:0:604800` | 805 / 92 | 912 | 见正文 |
| 安静开始 / `proactive.quiet_hours.start` | `"23:00"` / `time` | 806 / 通用或未单列 | 987 | 见正文 |
| 安静结束 / `proactive.quiet_hours.end` | `"08:00"` / `time` | 807 / 通用或未单列 | 988 | 见正文 |
| 允许联系的会话 / `proactive.targets` | `QJsonArray{}` / `targets` | 808 / 91 | 592, 989 | 见正文 |

### 每日上限（三个上限相互独立）

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 每日回顾上限 / `proactive.daily_limit` | `3` / `num:0:999` | 811 / 93 | 910 | 见正文 |
| 每日自主唤醒上限 / `cognition.open_wake.max_per_day` | `12` / `num:0:96` | 812 / 107 | 974 | 见正文 |
| 每日主动联系上限（0 表示不限） / `proactive.contact_daily_limit` | `3` / `num:0:99` | 813 / 94 | 915 | 见正文 |

### 内部唤醒与快照

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用内部唤醒（OpenWake） / `cognition.open_wake.enabled` | `false` / `bool` | 816 / 105 | 108, 272, 362, 425, 908, 924, 952 | 见正文 |
| 允许内部唤醒联系主人 / `cognition.open_wake.allow_contact` | `false` / `bool` | 817 / 106 | 953 | 见正文 |
| 共享每日预算占比（%） / `cognition.open_wake.shared_budget_percent` | `30` / `num:1:100` | 818 / 108 | 976 | 见正文 |
| 认知快照提示词预算（字节） / `cognition.snapshot_budget_bytes` | `4096` / `num:2048:8192` | 819 / 109 | 981 | 见正文 |
| 启用统一认知上下文（B5 实验功能） / `cognition.context_actual_enabled` | `false` / `bool` | 820 / 110 | 984 | 见正文 |

### 高级设置：调度与失败退避

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 首次唤醒延迟（秒） / `cognition.open_wake.initial_delay_seconds` | `900` / `num:60:86400` | 823 / 111 | 955 | 见正文 |
| 默认唤醒间隔（秒） / `cognition.open_wake.default_interval_seconds` | `1800` / `num:300:86400` | 824 / 112 | 965 | 见正文 |
| 最小唤醒间隔（秒） / `cognition.open_wake.min_interval_seconds` | `300` / `num:300:86400` | 825 / 113 | 959 | 见正文 |
| 最大唤醒间隔（秒） / `cognition.open_wake.max_interval_seconds` | `86400` / `num:300:86400` | 826 / 114 | 961 | 见正文 |
| 单次唤醒最多模型步数 / `cognition.open_wake.max_model_steps_per_wake` | `3` / `num:1:8` | 827 / 115 | 973 | 见正文 |
| 到期后先让出的普通回顾次数 / `cognition.open_wake.max_non_open_jobs_when_due` | `3` / `num:0:20` | 828 / 116 | 971 | 见正文 |
| 回顾失败退避（秒，逗号分隔） / `cognition.review_backoff_seconds` | `QJsonArray{120, 300, 900}` / `ints` | 829 / 117 | 932 | 见正文 |
| 连续失败暂停阈值 / `cognition.max_review_failures` | `4` / `num:1:10` | 830 / 118 | 944 | 见正文 |

### 检索服务（高级诊断）

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用向量召回 / `memory.retrieval.embedding_enabled` | `false` / `bool` | 835 / 56 | 171, 386 | 见正文 |
| 向量服务（local_hash_test 仅用于诊断） / `memory.retrieval.embedding_provider` | `""` / `choice:=未指定\|dashscope=阿里云百炼\|local_hash_test=本地测试` | 836 / 57 | 173, 387 | 见正文 |
| 向量端点 / `memory.retrieval.embedding_endpoint` | `""` / `text` | 837 / 62 | 175, 388 | 见正文 |
| 向量密钥环境变量 / `memory.retrieval.embedding_api_key_env` | `"DASHSCOPE_API_KEY"` / `text` | 838 / 61 | 177, 389 | 见正文 |
| 向量模型 / `memory.retrieval.embedding_model` | `""` / `text` | 839 / 58 | 179, 390 | 见正文 |
| 向量维度（0 为自动） / `memory.retrieval.embedding_dimension` | `0` / `num:0:8192` | 840 / 59 | 181, 391 | 见正文 |
| 重排端点 / `memory.retrieval.rerank_endpoint` | `""` / `text` | 841 / 63 | 183, 392 | 见正文 |
| 重排模型 / `memory.retrieval.rerank_model` | `"qwen3-rerank"` / `text` | 842 / 60 | 185, 393 | 见正文 |

### 兼容性：Legacy Planner

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 启用第二版检索规划 / `memory.agent_plan_v2` | `true` / `bool` | 845 / 67 | 145, 400 | 见正文 |
| 规划触发方式 / `memory.agent_trigger` | `"memory_question"` / `choice:memory_question=回忆类问题\|always=每轮` | 846 / 66 | 147, 401 | 见正文 |

### 日志与诊断

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 日志文件夹 / `logging.directory` | `""` / `log_directory` | 849 / 99 | 57, 482, 571, 575 | 见正文 |
| 每次启动单独创建日志 / `logging.separate_runs` | `true` / `bool` | 850 / 103 | 58, 482, 572, 576 | 见正文 |
| 记录完整对话与模型请求 / `memory.debug_full_logs` | `false` / `bool` | 851 / 104 | 88, 355 | 见正文 |

### 昼夜切换

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 夜间开始 / `time.night_start` | `"22:00"` / `time` | 856 / 通用或未单列 | 251, 418 | 见正文 |
| 夜间结束 / `time.night_end` | `"07:00"` / `time` | 857 / 通用或未单列 | 253, 419 | 见正文 |

### 状态持续时间

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 脸红（秒） / `time.blush_lasting` | `30` / `num:1:3600` | 860 / 通用或未单列 | 260, 420 | 见正文 |
| 表情（秒） / `time.emotion_lasting` | `15` / `num:1:3600` | 861 / 通用或未单列 | 261, 421 | 见正文 |
| 距离（秒） / `time.distance_lasting` | `15` / `num:1:3600` | 862 / 通用或未单列 | 262, 422 | 见正文 |

### 角色提示词

|设置 / JSON 路径|默认 / UI 类型与范围|UI / 帮助行|解析候选行|运行引用候选|
|---|---|---|---|---|
| 角色提示词 / `prompt` | `""` / `multiline` | 867 / 通用或未单列 | 专用控件或未映射 | 见正文 |
