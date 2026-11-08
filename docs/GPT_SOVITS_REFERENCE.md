# GPT-SoVITS 参考资料

> 2026-11-09 合并原 GPT_SOVITS_REFERENCE.md 与 MODULE_CALL_DOC.md。下文保留本地服务参考和备选集成示例，未重新验证上游所有版本。当前应用使用 TTSProcessManager 启动外部整合包中的 api_v2.py，由 ApiTTSProvider 经 HTTP 调用。文中的 tts_cli.py、tts_daemon.py 是示例方案，不能据此判定已接入本项目。

当前配置见 [配置参考](CONFIG_REFERENCE.md)，应用流程见 [架构](ARCHITECTURE.md)。OutDoc/api_v2.py 作为原有参考文件保留，运行脚本位置由用户配置指定。

# GPT-SoVITS API 接口文档

## 概述

GPT-SoVITS 提供三个版本的Web API接口：

| 版本 | 文件 | 特点 |
|------|------|------|
| v1 | `api.py` | 基础版，功能简洁 |
| **v2** | **`api_v2.py`** | **增强版，支持流式、批量推理、多参数控制，已原生支持 v3/v3lora 模型** |
| v3 | `api_v3.py` | 兼容 v1/v2 的旧版 API，**已被 api_v2.py 取代**，仅保留供参考 |

> **重要：** 当前项目已更新，**`api_v2.py` 已原生支持 v3/v3lora 模型**。
> v3 模型会被自动识别并走 v3 推理流程（decode_encp + CFM + BigVGAN），输出 24000Hz。
> **推荐统一使用 `api_v2.py`**，无需再使用 `api_v3.py`。

---

## v1 / v2 / v3 API 差异对比

| 特性 | v1 (api.py) | v2 (api_v2.py) | v3 (api_v3.py) |
|------|-------------|----------------|----------------|
| 核心接口 | `/` | `/tts` | `/tts` |
| 流式返回 | 支持（基础） | 支持（完善） | 支持（完善） |
| 批量推理 | 不支持 | 支持 | 支持 |
| 文本切分方法 | 自定义标点 | 6种预设方法 | 6种预设方法 |
| 辅助参考音频 | 支持 | 支持 | 支持 |
| 语速控制 | 支持 | 支持 | 支持 |
| 随机种子 | 不支持 | 支持 | 支持 |
| 重复惩罚 | 不支持 | 支持 | 支持 |
| 并行推理 | 不支持 | 支持（v3 模型自动关闭） | 支持（v3 模型自动关闭） |
| 参数校验 | 手动检查 | Pydantic模型校验 | Pydantic模型校验 |
| 单独切换模型 | `/set_model` | `/set_gpt_weights` + `/set_sovits_weights` | `/set_gpt_weights` + `/set_sovits_weights` |
| 默认参考音频 | `/change_refer` | `/set_refer_audio` | `/set_refer_audio` |
| **v3/v3lora 模型** | 不支持 | **原生支持（自动识别）** | 支持（monkey-patch） |
| **输出采样率** | 32000Hz | v3=24000Hz / v1,v2=32000Hz | v3=24000Hz / v1,v2=32000Hz |
| **sample_steps 参数** | 无 | 有（v3 专用，默认 32） | 有（v3 专用，默认 32） |
| **super_sampling 参数** | 无 | 有（v3 专用，默认 False） | 无 |
| 默认 text_split_method | - | `cut5` | `cut0` |
| 实现方式 | 独立 | 原生集成 | monkey-patch 扩展 TTS 类 |

---

## 一、api_v2.py (推荐使用)

### 1.1 启动方式

```bash
python api_v2.py -a 127.0.0.1 -p 9880 -c GPT_SoVITS/configs/tts_infer.yaml
```

#### 启动参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-a` | 绑定地址 | `127.0.0.1` |
| `-p` | 绑定端口 | `9880` |
| `-c` | TTS配置文件路径 | `GPT_SoVITS/configs/tts_infer.yaml` |

### 1.2 接口列表

| 接口 | 方法 | 功能 |
|------|------|------|
| `/tts` | GET/POST | 语音合成（核心接口） |
| `/control` | GET | 命令控制（重启/退出） |
| `/set_refer_audio` | GET | 设置参考音频 |
| `/set_gpt_weights` | GET | 切换GPT模型权重 |
| `/set_sovits_weights` | GET | 切换SoVITS模型权重 |

---

### 1.3 TTS语音合成接口

**Endpoint:** `POST /tts`

#### 请求参数

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `text` | string | 是 | - | 待合成的文本 |
| `text_lang` | string | 是 | - | 文本语种（中文、英文、日文、韩文、粤语等） |
| `ref_audio_path` | string | 是 | - | 参考音频文件路径 |
| `aux_ref_audio_paths` | list | 否 | `[]` | 辅助参考音频路径，用于多说话人音色融合 |
| `prompt_text` | string | 否 | `""` | 参考音频对应的文本 |
| `prompt_lang` | string | 是 | - | 参考音频文本的语种 |
| `top_k` | int | 否 | `5` | Top-K采样参数 |
| `top_p` | float | 否 | `1.0` | Top-P采样参数 |
| `temperature` | float | 否 | `1.0` | 温度参数，控制生成多样性 |
| `text_split_method` | string | 否 | `"cut5"` | 文本切分方法，见下方说明 |
| `batch_size` | int | 否 | `1` | 批量推理大小 |
| `batch_threshold` | float | 否 | `0.75` | 批量切分阈值 |
| `split_bucket` | bool | 否 | `true` | 是否将批次拆分为多个桶（v3 模型自动关闭） |
| `speed_factor` | float | 否 | `1.0` | 语速控制因子 |
| `fragment_interval` | float | 否 | `0.3` | 音频片段间隔 |
| `seed` | int | 否 | `-1` | 随机种子，-1表示随机 |
| `media_type` | string | 否 | `"wav"` | 输出音频格式：`wav`、`ogg`、`aac`、`raw` |
| `streaming_mode` | bool/int | 否 | `false` | 流式返回模式（v3 模型自动回退到分段返回） |
| `parallel_infer` | bool | 否 | `true` | 是否使用并行推理（v3 模型自动关闭） |
| `repetition_penalty` | float | 否 | `1.35` | T2S模型重复惩罚参数 |
| `sample_steps` | int | 否 | `32` | **v3 专用**，CFM 采样步数，影响质量和速度（建议 16-64） |
| `super_sampling` | bool | 否 | `false` | **v3 专用**，超分辨率开关，开启后音质更好但速度变慢 |
| `overlap_length` | int | 否 | `2` | 流式模式下语义 token 重叠长度 |
| `min_chunk_length` | int | 否 | `16` | 流式模式下最小 chunk 长度 |

#### 请求示例 (POST)

```json
{
    "text": "先帝创业未半而中道崩殂，今天下三分，益州疲弊，此诚危急存亡之秋也。",
    "text_lang": "zh",
    "ref_audio_path": "archive_jingyuan_1.wav",
    "prompt_text": "我是「罗浮」云骑将军景元。不必拘谨，「将军」只是一时的身份，你称呼我景元便可。",
    "prompt_lang": "zh",
    "top_k": 5,
    "top_p": 1.0,
    "temperature": 1.0,
    "text_split_method": "cut5",
    "batch_size": 1,
    "speed_factor": 1.0,
    "streaming_mode": false,
    "media_type": "wav"
}
```

#### 请求示例 (GET)

```
http://127.0.0.1:9880/tts?text=Hello&text_lang=en&ref_audio_path=sample.wav&prompt_lang=en&prompt_text=Hi
```

#### 响应

| 状态码 | 说明 |
|--------|------|
| 200 | 成功，返回音频流 |
| 400 | 失败，返回JSON错误信息 |

**成功响应:** 直接返回音频流（Content-Type: `audio/wav` 或 `audio/ogg` 或 `audio/aac`）

**失败响应:**
```json
{
    "message": "错误描述",
    "Exception": "异常信息"
}
```

#### 参数校验规则（Pydantic模型 TTS_Request）

v2 API使用 Pydantic 模型 `TTS_Request` 进行参数校验：

| 校验项 | 规则 |
|--------|------|
| `text_lang` | 必须在支持的语种列表中 |
| `prompt_lang` | 必须在支持的语种列表中 |
| `media_type` | 仅支持 `wav`、`raw`、`ogg`、`aac` |
| `streaming_mode + media_type` | `ogg` 格式仅在流式模式下支持 |
| `text_split_method` | 必须是有效的切分方法名称 |

---

### 1.4 命令控制接口

**Endpoint:** `GET /control`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `command` | string | 是 | 命令：`restart`（重启）或 `exit`（退出） |

#### 请求示例

```
http://127.0.0.1:9880/control?command=restart
```

---

### 1.5 设置参考音频接口

**Endpoint:** `GET /set_refer_audio`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `refer_audio_path` | string | 是 | 参考音频文件路径 |

#### 请求示例

```
http://127.0.0.1:9880/set_refer_audio?refer_audio_path=sample.wav
```

#### 响应

```json
{
    "message": "success"
}
```

---

### 1.6 切换GPT模型接口

**Endpoint:** `GET /set_gpt_weights`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `weights_path` | string | 是 | GPT模型权重文件路径 |

#### 请求示例

```
http://127.0.0.1:9880/set_gpt_weights?weights_path=GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt
```

#### 响应

```json
{
    "message": "success"
}
```

---

### 1.7 切换SoVITS模型接口

**Endpoint:** `GET /set_sovits_weights`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `weights_path` | string | 是 | SoVITS模型权重文件路径 |

#### 请求示例

```
http://127.0.0.1:9880/set_sovits_weights?weights_path=GPT_SoVITS/pretrained_models/s2G488k.pth
```

#### 响应

```json
{
    "message": "success"
}
```

### 1.8 v3/v3lora 模型使用说明

`api_v2.py` 已原生支持 v3/v3lora 模型，加载和推理流程完全自动。

#### 自动识别机制

调用 `/set_sovits_weights` 切换模型时，系统会自动检测模型版本：
- **v1/v2 模型**：走原始 encp→cfm→decoder 流程，输出 32000Hz
- **v3 模型**：创建 `SynthesizerTrnV3`，初始化 BigVGAN 声码器，走 encp→cfm→BigVGAN 流程，输出 24000Hz
- **v3lora 模型**：先加载预训练 `s2Gv3.pth`，再通过 PEFT 创建 LoRA 层，加载 LoRA 权重后 merge
- **v4 模型**：创建 `SynthesizerTrnV3`，初始化 Generator 声码器，输出 48000Hz

#### v3 模型自动行为

| 参数 | 行为 |
|------|------|
| `parallel_infer=true` | 自动设为 `false`（v3 不支持并行推理） |
| `split_bucket=true` | 自动设为 `false`（v3 不支持分桶） |
| `streaming_mode=true` | 自动回退到 `return_fragment=True` 分段返回模式 |
| `t2s_model.infer_panel` | 自动切换为 `infer_panel_naive_batched` |
| `prompt_text` 为空 | 抛出错误（v3 强制要求参考文本） |

#### v3 模型请求示例

```json
{
    "text": "おやすみなさい、お兄ちゃん。",
    "text_lang": "ja",
    "ref_audio_path": "reference_audio/loving_01.wav",
    "prompt_text": "そっか…じゃあ、キスもちゃんと今度しようね",
    "prompt_lang": "ja",
    "text_split_method": "cut0",
    "sample_steps": 32,
    "super_sampling": false,
    "speed_factor": 1.0,
    "streaming_mode": false,
    "media_type": "wav"
}
```

#### v3 模型输出说明

| 项目 | 值 |
|------|-----|
| 输出采样率 | **24000 Hz**（v1/v2 为 32000 Hz） |
| 分段返回 | 支持（`return_fragment` 模式） |
| 流式返回 | 不支持（自动回退到分段返回） |
| 默认语速 | 1.0 |

#### sample_steps 参数调优

| 值 | 效果 | 适用场景 |
|----|------|----------|
| 16 | 最快，质量略降 | 实时对话 |
| 32 | 默认，平衡质量与速度 | 日常使用 |
| 64 | 最慢，质量最佳 | 成品音频生成 |

#### super_sampling 参数

设置为 `true` 可开启超分辨率，提升音质但合成速度显著变慢。建议仅在对音质有极致要求时开启。

---

## 二、api_v3.py (旧版，已被 api_v2.py 取代)

### 2.1 启动方式

```bash
python api_v3.py -a 127.0.0.1 -p 9880 -c GPT_SoVITS/configs/tts_infer.yaml
```

#### 启动参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-a` / `--bind_addr` | 绑定地址 | `127.0.0.1` |
| `-p` / `--port` | 绑定端口 | `9880` |
| `-c` / `--tts_config` | TTS配置文件路径 | `GPT_SoVITS/configs/tts_infer.yaml` |

### 2.2 接口列表

| 接口 | 方法 | 功能 |
|------|------|------|
| `/tts` | GET/POST | 语音合成（核心接口） |
| `/control` | GET | 命令控制（重启/退出） |
| `/set_refer_audio` | GET | 设置参考音频 |
| `/set_gpt_weights` | GET | 切换GPT模型权重 |
| `/set_sovits_weights` | GET | 切换SoVITS模型权重（支持 v3lora） |

### 2.3 v3 模型加载机制

`api_v3.py` 通过 monkey-patch 替换 `TTS.init_vits_weights` 和 `TTS.run`，不修改上游 `TTS.py`：

- **v1/v2 模型**：走原始 `torch.load` 加载流程
- **v3 模型**：使用 `SynthesizerTrnV3` 类，并初始化 BigVGAN 声码器
- **v3lora 模型**：先加载预训练 `s2Gv3.pth`，再通过 PEFT 创建 LoRA 层，加载 LoRA 权重后调用 `merge_and_unload()` 合并

切换模型时若路径指向 `SoVITS_weights_v3/*.pth`，会自动识别为 v3 模型。

### 2.4 TTS语音合成接口

**Endpoint:** `POST /tts`（也支持 GET）

#### 请求参数

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `text` | string | 是 | - | 待合成的文本 |
| `text_lang` | string | 是 | - | 文本语种 |
| `ref_audio_path` | string | 是 | - | 参考音频文件路径 |
| `aux_ref_audio_paths` | list | 否 | `[]` | 辅助参考音频路径 |
| `prompt_text` | string | 否 | `""` | 参考音频对应的文本 |
| `prompt_lang` | string | 是 | - | 参考音频文本的语种 |
| `top_k` | int | 否 | `5` | Top-K采样参数 |
| `top_p` | float | 否 | `1.0` | Top-P采样参数 |
| `temperature` | float | 否 | `1.0` | 温度参数 |
| `text_split_method` | string | 否 | `"cut0"` | 文本切分方法 |
| `batch_size` | int | 否 | `1` | 批量推理大小 |
| `batch_threshold` | float | 否 | `0.75` | 批量切分阈值 |
| `split_bucket` | bool | 否 | `true` | v3 模型会自动关闭 |
| `speed_factor` | float | 否 | `1.0` | 语速控制因子 |
| `fragment_interval` | float | 否 | `0.3` | 音频片段间隔 |
| `seed` | int | 否 | `-1` | 随机种子 |
| `media_type` | string | 否 | `"wav"` | 输出格式：`wav`、`ogg`、`aac`、`raw` |
| `streaming_mode` | bool | 否 | `false` | 是否启用流式返回 |
| `parallel_infer` | bool | 否 | `true` | v3 模型会自动关闭 |
| `repetition_penalty` | float | 否 | `1.35` | T2S 重复惩罚 |
| `sample_steps` | int | 否 | `32` | **v3 专用**，CFM 采样步数，影响质量和速度（建议 16-64） |

#### 请求示例 (POST)

```json
{
    "text": "先帝创业未半而中道崩殂，今天下三分，益州疲弊，此诚危急存亡之秋也。",
    "text_lang": "zh",
    "ref_audio_path": "archive_jingyuan_1.wav",
    "prompt_text": "我是「罗浮」云骑将军景元。",
    "prompt_lang": "zh",
    "top_k": 5,
    "top_p": 1.0,
    "temperature": 1.0,
    "text_split_method": "cut0",
    "speed_factor": 1.0,
    "sample_steps": 32,
    "streaming_mode": false,
    "media_type": "wav"
}
```

#### 响应

| 状态码 | 说明 |
|--------|--------|
| 200 | 成功，返回音频流（v3 采样率 24000Hz） |
| 400 | 失败，返回 JSON 错误信息 |

**流式响应：** 启用 `streaming_mode=true` 时，wav 格式会先发送 24000Hz 的 wav header，随后持续推送音频片段；ogg/aac 直接流式推送。

#### v3 模型自动行为

| 参数 | 行为 |
|------|------|
| `parallel_infer=true` | 自动设为 `false`（v3 不支持并行推理） |
| `split_bucket=true` | 自动设为 `false`（v3 不支持分桶） |
| `t2s_model.infer_panel` | 自动切换为 `infer_panel_naive_batched` |

### 2.5 切换 SoVITS 模型（支持 v3lora）

**Endpoint:** `GET /set_sovits_weights`

```
http://127.0.0.1:9880/set_sovits_weights?weights_path=SoVITS_weights_v3/xxx.pth
```

加载 v3lora 模型时，会自动：
1. 加载预训练模型 `GPT_SoVITS/pretrained_models/s2Gv3.pth`
2. 通过 PEFT 创建 LoRA 层（target_modules: to_k/to_q/to_v/to_out.0）
3. 加载用户 LoRA 权重
4. 调用 `merge_and_unload()` 合并权重，恢复为标准 `SynthesizerTrnV3`

其他控制接口（`/control`、`/set_refer_audio`、`/set_gpt_weights`）用法与 api_v2.py 完全一致，参见第一章。

---

## 三、api.py (基础版)

### 3.1 启动方式

```bash
python api.py -dr "123.wav" -dt "一二三。" -dl "zh"
```

#### 启动参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-s` | SoVITS模型路径 | 从config读取 |
| `-g` | GPT模型路径 | 从config读取 |
| `-dr` | 默认参考音频路径 | 空 |
| `-dt` | 默认参考音频文本 | 空 |
| `-dl` | 默认参考音频语种 | 空 |
| `-d` | 推理设备 | `cuda` |
| `-a` | 绑定地址 | `127.0.0.1` |
| `-p` | 绑定端口 | `9880` |
| `-fp` | 使用全精度 | `false` |
| `-hp` | 使用半精度 | `false` |
| `-sm` | 流式返回模式 | `close` |
| `-mt` | 音频编码格式 | `wav` |
| `-st` | 音频数据类型 | `int16` |
| `-cp` | 文本切分符号 | 空 |
| `-hb` | CNHubert路径 | 从config读取 |
| `-b` | BERT路径 | 从config读取 |

### 3.2 接口列表

| 接口 | 方法 | 功能 |
|------|------|------|
| `/` | GET/POST | 语音合成 |
| `/set_model` | GET/POST | 设置模型 |
| `/change_refer` | GET/POST | 更换默认参考音频 |
| `/control` | GET/POST | 命令控制 |

---

### 3.3 TTS语音合成接口

**Endpoint:** `POST /`

#### 请求参数

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `text` | string | 是 | - | 待合成的文本 |
| `text_language` | string | 是 | - | 文本语种 |
| `refer_wav_path` | string | 否 | - | 参考音频路径（未指定则使用默认） |
| `prompt_text` | string | 否 | - | 参考音频文本（未指定则使用默认） |
| `prompt_language` | string | 否 | - | 参考音频语种（未指定则使用默认） |
| `cut_punc` | string | 否 | - | 文本切分符号 |
| `top_k` | int | 否 | `15` | Top-K采样参数 |
| `top_p` | float | 否 | `1.0` | Top-P采样参数 |
| `temperature` | float | 否 | `1.0` | 温度参数 |
| `speed` | float | 否 | `1.0` | 语速控制 |
| `inp_refs` | list | 否 | `[]` | 额外参考音频路径列表 |

#### 请求示例 (POST)

```json
{
    "refer_wav_path": "123.wav",
    "prompt_text": "一二三。",
    "prompt_language": "zh",
    "text": "先帝创业未半而中道崩殂，今天下三分，益州疲弊，此诚危急存亡之秋也。",
    "text_language": "zh",
    "top_k": 20,
    "top_p": 0.6,
    "temperature": 0.6,
    "speed": 1.0,
    "inp_refs": ["456.wav", "789.wav"]
}
```

#### 请求示例 (GET)

```
http://127.0.0.1:9880?text=Hello&text_language=en&refer_wav_path=sample.wav&prompt_text=Hi&prompt_language=en
```

#### 响应

| 状态码 | 说明 |
|--------|------|
| 200 | 成功，返回音频流 |
| 400 | 失败，返回JSON错误信息 |

---

### 3.4 更换默认参考音频接口

**Endpoint:** `POST /change_refer`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `refer_wav_path` | string | 是 | 参考音频路径 |
| `prompt_text` | string | 是 | 参考音频文本 |
| `prompt_language` | string | 是 | 参考音频语种 |

#### 请求示例

```json
{
    "refer_wav_path": "123.wav",
    "prompt_text": "一二三。",
    "prompt_language": "zh"
}
```

#### 响应

```json
{
    "code": 0,
    "message": "Success"
}
```

---

### 3.5 设置模型接口

**Endpoint:** `POST /set_model`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `gpt_model_path` | string | 是 | GPT模型路径 |
| `sovits_model_path` | string | 是 | SoVITS模型路径 |

#### 请求示例

```json
{
    "gpt_model_path": "GPT_SoVITS/pretrained_models/s1bert25hz.ckpt",
    "sovits_model_path": "GPT_SoVITS/pretrained_models/s2G488k.pth"
}
```

---

### 3.6 命令控制接口

**Endpoint:** `POST /control`

#### 请求参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `command` | string | 是 | `restart` 或 `exit` |

---

## 四、文本切分方法说明

v2 / v3 API 提供6种文本切分方法（定义在 `GPT_SoVITS/TTS_infer_pack/text_segmentation_method.py`）：

| 方法名 | 说明 | 适用场景 |
|--------|------|----------|
| `cut0` | 不切分，原样返回 | 短文本、已手动分段的文本 |
| `cut1` | 凑4句一切 | 按完整句子分组，保持语义完整 |
| `cut2` | 凑50字一切 | 按字数切分，平衡长度和语义 |
| `cut3` | 按中文句号「。」切 | 纯中文文本 |
| `cut4` | 按英文句号「.」切 | 纯英文文本（排除数字中的点） |
| `cut5` | 按标点符号切（默认） | 通用方法，支持中英文标点，排除数字中的点 |

**cut5 支持的标点符号:** `,` `.` `;` `?` `!` `、` `，` `。` `？` `！` `：` `…`

---

## 五、支持的语种

| 语种代码 | 说明 |
|----------|------|
| `zh` | 中文（中英混合） |
| `en` | 英文 |
| `ja` | 日文（日英混合） |
| `ko` | 韩文（韩英混合） |
| `yue` | 粤语（粤英混合） |
| `auto` | 多语种混合（自动识别） |
| `auto_yue` | 多语种混合（粤语模式） |
| `all_zh` | 纯中文 |
| `all_yue` | 纯粤语 |
| `all_ja` | 纯日文 |
| `all_ko` | 纯韩文 |

---

## 六、音频格式说明

| 格式 | 支持流式 | 说明 |
|------|----------|------|
| `wav` | 否 | 默认格式，无损 |
| `ogg` | 是 | 流式默认格式，有损压缩 |
| `aac` | 是 | 有损压缩，需要FFmpeg |

---

## 七、采样率说明

不同模型版本输出的音频采样率不同，调用方需根据采样率正确播放：

| 模型版本 | API 文件 | 输出采样率 | 流式 wav header |
|----------|----------|------------|-----------------|
| v1 / v2 | api.py / api_v2.py | 32000 Hz | 32000 Hz |
| v3 / v3lora | **api_v2.py** | **24000 Hz** | **24000 Hz** |
| v4 | api_v2.py | 48000 Hz | 48000 Hz |

**注意：**
- `api_v2.py` 加载 v3/v3lora 模型时，自动输出 24000 Hz
- `api_v2.py` 加载 v1/v2 模型时，仍输出 32000 Hz
- `api_v2.py` 加载 v4 模型时，输出 48000 Hz
- 流式 wav 响应的 header 采样率与音频实际采样率一致，播放器应动态读取 header，不要硬编码采样率
- **v3 模型超分模式**：v3/v3lora 模型开启 `super_sampling=true` 时，输出采样率从 24000 Hz 提升到 48000 Hz（与 v4 一致）
- 调用方需根据 `super_sampling` 参数动态适配播放器采样率，不能假设 v3 模型固定为 24000 Hz

---

## 八、配置文件说明

`config.py` 中定义了影响API行为的全局配置：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `sovits_path` | `""` | SoVITS模型路径（空则使用预训练模型） |
| `gpt_path` | `""` | GPT模型路径（空则使用预训练模型） |
| `is_half` | `True` | 是否使用半精度推理（GPU自动检测） |
| `cnhubert_path` | `GPT_SoVITS/pretrained_models/chinese-hubert-base` | CNHubert模型路径 |
| `bert_path` | `GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large` | BERT模型路径 |
| `pretrained_sovits_path` | `GPT_SoVITS/pretrained_models/s2G488k.pth` | 预训练SoVITS模型路径 |
| `pretrained_gpt_path` | `GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt` | 预训练GPT模型路径 |
| `infer_device` | `cuda`（可用时） | 推理设备（cuda/cpu） |
| `api_port` | `9880` | API服务端口 |

### 半精度自动检测规则

| 条件 | 行为 |
|------|------|
| GPU型号含 `16`（非V100）、`P40`、`P10`、`1060`、`1070`、`1080` | 强制使用全精度 |
| CPU推理 | 强制使用全精度 |
| 其他GPU | 使用半精度 |

---



---

# GPT-SoVITS 模块直接调用文档

## 概述

本文档介绍如何直接通过Python模块调用GPT-SoVITS进行语音合成，适用于Qt/C++项目通过QProcess调用的场景。

提供三种调用方式：
1. **命令行单次调用** (`tts_cli.py`)：简单直接，每次调用重新加载模型
2. **常驻后台服务** (`tts_daemon.py`)：启动时加载模型，持续待命，支持流式输出和音频缓存
3. **Python模块直接调用**：直接导入TTS模块进行编程式调用

### 模型版本说明（重要）

GPT-SoVITS 支持多种模型版本，**当前项目已更新，v3/v3lora 模型已原生支持**：

| 模型版本 | 模型目录 | 调用方式 |
|----------|----------|----------|
| v1 / v2 | `GPT_SoVITS/pretrained_models/`、`SoVITS_weights_v2/` | 直接使用 `tts_daemon.py` 或导入 `TTS` 类 |
| **v3 / v3lora** | `SoVITS_weights_v3/` | **原生支持，直接使用 `tts_daemon.py` 或导入 `TTS` 类即可** |
| v4 | 训练产出的 v4 模型 | 原生支持，自动走 Generator 声码器流程 |

> **更新说明：** 项目已更新，`TTS.init_vits_weights` 和 `TTS.run` 已原生集成 v3 推理流程（encp + CFM + BigVGAN），
> 无需额外 monkey-patch。v3 模型会被自动识别，加载时自动合并 LoRA 权重，推理时自动切换到 BigVGAN 声码器。

**两种集成方案：**

| 方案 | 说明 | 适用场景 |
|------|------|----------|
| 方案A：HTTP 调用 | Qt/C++ 启动 `api_v2.py` 作为 HTTP 服务，通过 `/tts` 接口调用 | 推荐：解耦清晰，自动支持 v1/v2/v3/v4 |
| 方案B：进程内调用 | Qt/C++ 启动 `tts_daemon.py`，通过 stdin/stdout JSON 通信 | 需要使用 stdin/stdout JSON 通信的场景 |

---

## 重要：Python环境说明

GPT-SoVITS 整合包自带完整的 Python 运行环境，位于 GPT-SoVITS 项目目录下的 `runtime\` 文件夹中。**必须使用此自带的 Python 环境**，不要使用系统 Python。

### Python 可执行文件路径

```
<GPT-SoVITS项目路径>\runtime\python.exe
```

### 示例

假设 GPT-SoVITS 项目路径为 `D:\Tools\GPT_SoVITS_v3lora\GPT-SoVITS-v3lora-20250228`，则：

```
D:\Tools\GPT_SoVITS_v3lora\GPT-SoVITS-v3lora-20250228\runtime\python.exe
```

### 在 Qt/C++ 中调用

```cpp
QString pythonPath = gptSovitsPath + "/runtime/python.exe";
QString scriptPath = gptSovitsPath + "/tts_daemon.py";
// 使用 pythonPath 启动 scriptPath
```

---

## 一、常驻后台服务（推荐使用）

### 1.1 特点

- 启动时加载模型，持续待命，避免每次合成都重新加载（首次加载约30-60秒）
- 支持流式音频输出，实时返回音频片段
- 支持同时将音频缓存到本地
- 通过stdin/stdout进行JSON通信，无需HTTP服务
- **v3/v3lora 模型支持：** 原生支持，直接加载 v3 模型即可自动走 v3 推理流程（BigVGAN 声码器，24000Hz）

### 1.2 启动方式

**必须使用整合包自带的 runtime\python.exe，不要使用系统 python。**

```bash
"<GPT-SoVITS项目路径>\runtime\python.exe" tts_daemon.py --gpt_sovits_path "<GPT-SoVITS项目路径>" --gpt_model "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt" --sovits_model "GPT_SoVITS/pretrained_models/s2G488k.pth"
```

**示例：**

```bash
"D:\Tools\GPT_SoVITS_v3lora\GPT-SoVITS-v3lora-20250228\runtime\python.exe" tts_daemon.py --gpt_sovits_path "D:\Tools\GPT_SoVITS_v3lora\GPT-SoVITS-v3lora-20250228" --gpt_model "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt" --sovits_model "GPT_SoVITS/pretrained_models/s2G488k.pth"
```

### 1.3 启动参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--gpt_sovits_path` | (必填) | GPT-SoVITS项目绝对路径 |
| `--gpt_model` | `GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt` | GPT模型路径 |
| `--sovits_model` | `GPT_SoVITS/pretrained_models/s2G488k.pth` | SoVITS模型路径 |
| `--device` | `cuda` | 推理设备 |
| `--is_half` | `false` | 使用半精度推理 |
| `--version` | `v2` | 模型版本 |
| `--bert_path` | `GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large` | BERT模型路径 |
| `--cnhubert_path` | `GPT_SoVITS/pretrained_models/chinese-hubert-base` | CNHuBERT模型路径 |

### 1.4 通信协议

服务启动后，通过标准输入输出进行JSON通信：
- **stdin**：接收命令，每行一条JSON消息
- **stdout**：仅输出JSON响应（每行一条），模型内部的日志已被自动过滤
- **stderr**：输出模型加载和推理过程中的日志信息（可用于调试）

这种分离机制确保了 stdout 上的数据都是合法的 JSON，便于 C++ 端解析。

> **采样率：** v1/v2 模型输出 32000Hz，v3/v3lora 模型输出 24000Hz。每个 `audio_fragment` 都会携带 `sampling_rate` 字段，调用方应动态读取，不要硬编码。

#### 启动响应

```json
{
    "type": "initialized",
    "success": true,
    "message": "TTS服务已就绪，等待命令..."
}
```

#### 命令列表

| 命令 | 说明 |
|------|------|
| `list_models` | 获取模型列表 |
| `load_model` | 切换模型 |
| `tts` | 流式语音合成（不缓存） |
| `tts_cached` | 流式语音合成（同时缓存） |
| `exit` | 退出服务 |

### 1.5 获取模型列表

**输入：**
```json
{"command": "list_models"}
```

**输出：**
```json
{
    "type": "model_list",
    "success": true,
    "models": [
        {"name": "s1bert25hz-2kh-longer-epoch=68e-step=50232", "type": "gpt", "path": "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt", "absolute_path": "..."},
        {"name": "s2G488k", "type": "sovits", "path": "GPT_SoVITS/pretrained_models/s2G488k.pth", "absolute_path": "..."}
    ]
}
```

### 1.6 切换模型

**输入：**
```json
{
    "command": "load_model",
    "gpt_model": "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt",
    "sovits_model": "GPT_SoVITS/pretrained_models/s2G488k.pth"
}
```

**输出：**
```json
{
    "type": "model_loaded",
    "success": true,
    "message": "模型切换成功"
}
```

### 1.7 流式语音合成（不缓存）

**输入：**
```json
{
    "command": "tts",
    "text": "你好，这是一个测试",
    "text_lang": "zh",
    "ref_audio": "archive_jingyuan_1.wav",
    "prompt_text": "我是景元",
    "prompt_lang": "zh",
    "top_k": 5,
    "top_p": 1.0,
    "temperature": 1.0,
    "speed_factor": 1.0
}
```

**输出（流式）：**

首先是音频片段（多次输出）：
```json
{
    "type": "audio_fragment",
    "sampling_rate": 32000,
    "chunk_size": 64000,
    "audio_data": "..."
}
```

最后是完成信息：
```json
{
    "type": "tts_complete",
    "success": true,
    "sampling_rate": 32000,
    "total_length": 192000,
    "cache_path": null,
    "message": "语音合成完成"
}
```

### 1.8 流式语音合成（带缓存）

**输入：**
```json
{
    "command": "tts_cached",
    "text": "你好，这是一个测试",
    "text_lang": "zh",
    "ref_audio": "archive_jingyuan_1.wav",
    "prompt_text": "我是景元",
    "prompt_lang": "zh",
    "cache_path": "D:\\chat_project\\audio_cache\\test.wav"
}
```

**输出：** 同流式合成，但完成信息中包含缓存路径。

### 1.9 音频数据格式

音频片段中的 `audio_data` 字段是原始音频数据的十六进制编码（hex），解码方式：

```python
import binascii
audio_bytes = binascii.unhexlify(audio_data)
```

### 1.10 v3 / v3lora 模型 tts 命令示例

加载 v3 模型后，tts 命令可新增 `sample_steps` 参数（CFM 采样步数，默认 32，建议 16-64）和 `super_sampling` 参数：

**输入：**
```json
{
    "command": "tts_cached",
    "text": "你好，这是一个 v3 模型测试",
    "text_lang": "zh",
    "ref_audio": "archive_jingyuan_1.wav",
    "prompt_text": "我是景元",
    "prompt_lang": "zh",
    "sample_steps": 32,
    "super_sampling": false,
    "cache_path": "D:\\chat_project\\audio_cache\\v3_test.wav"
}
```

**输出：** 同 1.7/1.8，但 `sampling_rate` 为 **24000**（`super_sampling=false` 时）或 **48000**（`super_sampling=true` 时）。

**注意：**
- v3 模型会自动忽略 `parallel_infer` 和 `split_bucket` 参数（强制关闭），无需手动设置
- v3 模型不支持流式返回，`streaming_mode` 会被自动回退到分段返回
- `sample_steps` 建议范围 16-64：16 最快质量略降，32 默认平衡，64 最慢质量最佳
- `super_sampling` 开启后音质更好但速度显著变慢
- **采样率动态变化**：`super_sampling=true` 时输出采样率从 24000Hz 提升到 48000Hz，调用方应动态读取每个 `audio_fragment` 和 `tts_complete` 中的 `sampling_rate` 字段，不要硬编码 24000

---

## 二、命令行单次调用

### 2.1 获取模型列表

```bash
"<GPT-SoVITS项目路径>\runtime\python.exe" tts_cli.py --action list_models --gpt_sovits_path "<GPT-SoVITS项目路径>"
```

**示例：**

```bash
"D:\Tools\GPT_SoVITS_v3lora\GPT-SoVITS-v3lora-20250228\runtime\python.exe" tts_cli.py --action list_models --gpt_sovits_path "D:\Tools\GPT_SoVITS_v3lora\GPT-SoVITS-v3lora-20250228"
```

### 2.2 执行语音合成

```bash
"<GPT-SoVITS项目路径>\runtime\python.exe" tts_cli.py --action tts --gpt_sovits_path "<项目路径>" --text "<文本>" --gpt_model "<GPT模型路径>" --sovits_model "<SoVITS模型路径>" --output "<输出文件>"
```

**参数说明：**

| 参数 | 必填 | 说明 |
|------|------|------|
| `--action` | 是 | 固定为 `tts` |
| `--gpt_sovits_path` | 是 | GPT-SoVITS项目绝对路径 |
| `--text` | 是 | 待合成的文本 |
| `--gpt_model` | 是 | GPT模型路径 |
| `--sovits_model` | 是 | SoVITS模型路径 |
| `--output` | 是 | 输出音频文件路径 |
| `--text_lang` | 否 | 文本语种，默认 `zh` |
| `--ref_audio` | 否 | 参考音频路径 |
| `--prompt_text` | 否 | 参考音频对应的文本 |
| `--prompt_lang` | 否 | 参考音频文本的语种 |

---

## 三、Python模块直接调用

### 3.1 基本步骤

1. 设置 `sys.path`
2. 导入 `TTS` 和 `TTS_Config`
3. 创建配置和实例（模型加载）
4. 执行推理

### 3.2 代码示例（普通模式）

```python
import os
import sys
import soundfile as sf

# 1. 设置sys.path
gpt_sovits_path = "D:\\Tools\\GPT_SoVITS_v3lora\\GPT-SoVITS-v3lora-20250228"
sys.path.insert(0, gpt_sovits_path)
sys.path.insert(0, os.path.join(gpt_sovits_path, "GPT_SoVITS"))

# 2. 导入模块
from GPT_SoVITS.TTS_infer_pack.TTS import TTS, TTS_Config

# 3. 创建配置
config_dict = {
    "device": "cuda",
    "is_half": True,
    "version": "v2",
    "t2s_weights_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt"),
    "vits_weights_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/s2G488k.pth"),
    "bert_base_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large"),
    "cnhuhbert_base_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/chinese-hubert-base"),
}
tts_config = TTS_Config(config_dict)

# 4. 创建TTS实例（加载模型）
tts_pipeline = TTS(tts_config)

# 5. 构建推理请求
req = {
    "text": "你好，这是一个测试",
    "text_lang": "zh",
    "ref_audio_path": os.path.join(gpt_sovits_path, "archive_jingyuan_1.wav"),
    "prompt_text": "我是景元",
    "prompt_lang": "zh",
    "top_k": 5,
    "top_p": 1.0,
    "temperature": 1.0,
    "text_split_method": "cut5",
    "batch_size": 1,
    "speed_factor": 1.0,
    "seed": -1,
}

# 6. 执行推理
tts_generator = tts_pipeline.run(req)
sr, audio_data = next(tts_generator)

# 7. 保存音频
sf.write("output.wav", audio_data, sr, format='wav')
```

### 3.3 代码示例（流式模式）

```python
import os
import sys
import soundfile as sf
import numpy as np

# 设置sys.path
gpt_sovits_path = "D:\\Tools\\GPT_SoVITS_v3lora\\GPT-SoVITS-v3lora-20250228"
sys.path.insert(0, gpt_sovits_path)
sys.path.insert(0, os.path.join(gpt_sovits_path, "GPT_SoVITS"))

from GPT_SoVITS.TTS_infer_pack.TTS import TTS, TTS_Config

# 创建配置和实例
config_dict = {
    "device": "cuda",
    "is_half": True,
    "version": "v2",
    "t2s_weights_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt"),
    "vits_weights_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/s2G488k.pth"),
    "bert_base_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large"),
    "cnhuhbert_base_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/chinese-hubert-base"),
}
tts_config = TTS_Config(config_dict)
tts_pipeline = TTS(tts_config)

# 构建推理请求（流式模式）
req = {
    "text": "你好，这是一个较长的测试文本，用于演示流式输出。",
    "text_lang": "zh",
    "ref_audio_path": os.path.join(gpt_sovits_path, "archive_jingyuan_1.wav"),
    "prompt_text": "我是景元",
    "prompt_lang": "zh",
    "top_k": 5,
    "top_p": 1.0,
    "temperature": 1.0,
    "text_split_method": "cut5",
    "batch_size": 1,
    "speed_factor": 1.0,
    "seed": -1,
    "return_fragment": True,  # 开启流式返回
    "fragment_interval": 0.3,
}

# 执行推理（流式）
tts_generator = tts_pipeline.run(req)

all_audio = []
for sr, chunk in tts_generator:
    # 实时处理音频片段
    print(f"收到音频片段: {len(chunk)} 样本，采样率: {sr}")
    
    # 将片段发送给播放器（这里可以直接写入音频流）
    # play_chunk(chunk, sr)
    
    all_audio.append(chunk)

# 合并所有片段并保存
full_audio = np.concatenate(all_audio)
sf.write("output_streaming.wav", full_audio, sr, format='wav')
```

### 3.4 代码示例（v3 / v3lora 模型）

**关键差异：**
- v3/v3lora 模型已原生支持，**无需额外 `import api_v3`**
- `vits_weights_path` 指向 `SoVITS_weights_v3/*.pth`
- 请求中可新增 `sample_steps` 参数（v3 专用，默认 32）和 `super_sampling` 参数
- 输出采样率为 **24000 Hz**（非 32000）

```python
import os
import sys
import soundfile as sf

# 1. 设置sys.path
gpt_sovits_path = "D:\\Tools\\GPT_SoVITS_v3lora\\GPT-SoVITS-v3lora-20250228"
sys.path.append(gpt_sovits_path)
sys.path.append(os.path.join(gpt_sovits_path, "GPT_SoVITS"))

# 2. 直接导入 TTS（v3 原生支持，无需 monkey-patch）
from GPT_SoVITS.TTS_infer_pack.TTS import TTS, TTS_Config

# 3. 创建配置（vits_weights_path 指向 v3 模型）
config_dict = {
    "device": "cuda",
    "is_half": True,
    "version": "v2",  # 配置文件仍写 v2，TTS 会自动识别 v3 模型
    "t2s_weights_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt"),
    "vits_weights_path": os.path.join(gpt_sovits_path, "SoVITS_weights_v3/千岛茉子.pth"),
    "bert_base_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large"),
    "cnhuhbert_base_path": os.path.join(gpt_sovits_path, "GPT_SoVITS/pretrained_models/chinese-hubert-base"),
}
tts_config = TTS_Config(config_dict)
tts_pipeline = TTS(tts_config)  # 自动识别 v3 模型，加载 LoRA 并初始化 BigVGAN

# 4. 构建推理请求（含 sample_steps）
req = {
    "text": "你好，这是 v3 LoRA 模型测试",
    "text_lang": "zh",
    "ref_audio_path": os.path.join(gpt_sovits_path, "archive_jingyuan_1.wav"),
    "prompt_text": "我是景元",
    "prompt_lang": "zh",
    "top_k": 5,
    "top_p": 1.0,
    "temperature": 1.0,
    "text_split_method": "cut0",
    "batch_size": 1,
    "speed_factor": 1.0,
    "seed": -1,
    "sample_steps": 32,      # v3 专用：CFM 采样步数
    "super_sampling": False, # v3 专用：超分辨率
    # parallel_infer / split_bucket 会被自动关闭，无需手动设置
}

# 5. 执行推理
tts_generator = tts_pipeline.run(req)
sr, audio_data = next(tts_generator)
print(f"采样率: {sr}")  # 输出 24000

# 6. 保存音频
sf.write("output_v3.wav", audio_data, sr, format='wav')
```

> **注意：** v3/v3lora 模型已原生支持，`TTS.init_vits_weights` 会自动识别 v3 模型版本并走对应加载流程（含 LoRA 合并和 BigVGAN 初始化）。
> `TTS.run` 会自动检测 `use_vocoder` 标志，走 v3 推理流程。

### 3.5 TTS_Config 构造参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `device` | string | 否 | 推理设备，默认自动检测 |
| `is_half` | bool | 否 | 是否使用半精度，默认自动检测 |
| `version` | string | 否 | 配置文件版本：`v1` 或 `v2`，默认 `v2`（v3 模型由 `api_v3` 自动识别，无需在此指定） |
| `t2s_weights_path` | string | 是 | GPT/T2S模型路径 |
| `vits_weights_path` | string | 是 | SoVITS/VITS模型路径（v3 模型指向 `SoVITS_weights_v3/*.pth`） |
| `bert_base_path` | string | 是 | BERT模型目录路径 |
| `cnhuhbert_base_path` | string | 是 | CNHubert模型目录路径 |

### 3.6 TTS.run() 请求参数

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `text` | string | 是 | - | 待合成的文本 |
| `text_lang` | string | 是 | - | 文本语种 |
| `ref_audio_path` | string | 是 | - | 参考音频路径 |
| `prompt_text` | string | 否 | `""` | 参考音频对应的文本 |
| `prompt_lang` | string | 是 | - | 参考音频文本的语种 |
| `top_k` | int | 否 | `5` | Top-K采样参数 |
| `top_p` | float | 否 | `1.0` | Top-P采样参数 |
| `temperature` | float | 否 | `1.0` | 温度参数 |
| `text_split_method` | string | 否 | `"cut5"` | 文本切分方法 |
| `batch_size` | int | 否 | `1` | 批量推理大小 |
| `speed_factor` | float | 否 | `1.0` | 语速控制因子 |
| `seed` | int | 否 | `-1` | 随机种子 |
| `return_fragment` | bool | 否 | `false` | 是否流式返回 |
| `fragment_interval` | float | 否 | `0.3` | 音频片段间隔（秒） |
| `sample_steps` | int | 否 | `32` | **v3 专用**，CFM 采样步数（建议 16-64） |
| `super_sampling` | bool | 否 | `false` | **v3 专用**，超分辨率开关，开启后音质更好但速度变慢 |
| `streaming_mode` | bool/int | 否 | `false` | 流式返回模式（v3 模型自动回退到分段返回） |
| `overlap_length` | int | 否 | `2` | 流式模式下语义 token 重叠长度 |
| `min_chunk_length` | int | 否 | `16` | 流式模式下最小 chunk 长度 |

### 3.7 TTS.run() 返回值

返回一个生成器，每次迭代返回 `(sampling_rate, audio_data)` 元组：

| 返回项 | 类型 | 说明 |
|--------|------|------|
| `sampling_rate` | int | 采样率：v1/v2=32000，v3=24000 |
| `audio_data` | numpy.ndarray | 音频数据数组（int16） |

---

## 四、Qt/C++项目集成指南

### 4.1 配置文件格式

```json
{
    "gpt_sovits_path": "D:\\Tools\\GPT_SoVITS_v3lora\\GPT-SoVITS-v3lora-20250228",
    "selected_gpt_model": "GPT_SoVITS/pretrained_models/s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt",
    "selected_sovits_model": "SoVITS_weights_v3/千岛茉子.pth",
    "reference_audio_dir": "D:\\chat_project\\reference_audio",
    "audio_cache_dir": "D:\\chat_project\\audio_cache",
    "default_text_lang": "zh",
    "default_prompt_lang": "zh",
    "enable_tts": true,
    "use_v3_model": true,
    "sample_steps": 32,
    "tts_api_mode": "http"
}
```

**字段说明：**

| 字段 | 说明 |
|------|------|
| `use_v3_model` | 是否使用 v3/v3lora 模型。`true` 时启动 `api_v2.py`；`false` 时启动 `tts_daemon.py` |
| `sample_steps` | v3 专用参数，CFM 采样步数（默认 32，建议 16-64） |
| `tts_api_mode` | 调用方式：`http`（HTTP 调用 api_v2.py，支持所有模型版本）/ `daemon`（stdin/stdout 调用 tts_daemon.py） |

### 4.2 项目启动时启动TTS服务

> **启动脚本选择：**
> - 所有模型版本（含 v3/v3lora/v4）：启动 `api_v2.py`，通过 HTTP 调用接口（推荐）
> - v1/v2 模型：启动 `tts_daemon.py`，通过 stdin/stdout JSON 通信
> - v3/v3lora 模型已原生支持，`api_v2.py` 会自动识别模型版本并走对应推理流程

下方示例为 v1/v2 模型 + daemon 模式的完整代码。v3 模型请参见 4.4。

```cpp
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

class TTSService : public QObject {
    Q_OBJECT
    
public:
    TTSService(const QString& gptSovitsPath, QObject *parent = nullptr) : QObject(parent) {
        process = new QProcess(this);
        
        // 必须使用整合包自带的 runtime\python.exe
        QString pythonPath = gptSovitsPath + "/runtime/python.exe";
        QString scriptPath = gptSovitsPath + "/tts_daemon.py";
        
        // 设置进程参数
        QStringList args;
        args << scriptPath;
        args << "--gpt_sovits_path" << gptSovitsPath;
        args << "--gpt_model" << "GPT_SoVITS/pretrained_models/s1bert25hz-2kh-longer-epoch=68e-step=50232.ckpt";
        args << "--sovits_model" << "GPT_SoVITS/pretrained_models/s2G488k.pth";
        
        process->setProgram(pythonPath);
        process->setArguments(args);
        
        // 设置管道通信
        process->setProcessChannelMode(QProcess::SeparateChannels);
        
        // 连接信号
        connect(process, &QProcess::readyReadStandardOutput, this, &TTSService::onReadyRead);
        connect(process, &QProcess::readyReadStandardError, this, &TTSService::onErrorRead);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &TTSService::onFinished);
        
        // 启动进程
        process->start();
        qDebug() << "TTS服务启动中...";
    }
    
    ~TTSService() {
        // 发送退出命令
        if (process->state() == QProcess::Running) {
            QString exitCmd = QString("{\"command\": \"exit\"}\n");
            process->write(exitCmd.toUtf8());
            process->waitForFinished(3000);
        }
        process->deleteLater();
    }
    
    // 获取模型列表
    void requestModelList() {
        QString cmd = QString("{\"command\": \"list_models\"}\n");
        process->write(cmd.toUtf8());
    }
    
    // 切换模型
    void switchModel(const QString& gptModel, const QString& sovitsModel) {
        QJsonObject obj;
        obj["command"] = "load_model";
        obj["gpt_model"] = gptModel;
        obj["sovits_model"] = sovitsModel;
        QString cmd = QString(QJsonDocument(obj).toJson(QJsonDocument::Compact)) + "\n";
        process->write(cmd.toUtf8());
    }
    
    // 流式语音合成（带缓存）
    void synthesize(const QString& text, const QString& refAudio, const QString& cachePath) {
        QJsonObject obj;
        obj["command"] = "tts_cached";
        obj["text"] = text;
        obj["text_lang"] = "zh";
        obj["ref_audio"] = refAudio;
        obj["prompt_text"] = "";
        obj["prompt_lang"] = "zh";
        obj["cache_path"] = cachePath;
        QString cmd = QString(QJsonDocument(obj).toJson(QJsonDocument::Compact)) + "\n";
        process->write(cmd.toUtf8());
    }
    
signals:
    void modelListReceived(QJsonArray models);
    void audioFragmentReceived(int samplingRate, const QByteArray& audioData);
    void ttsCompleted(bool success, const QString& cachePath);
    void errorOccurred(const QString& message);
    
private slots:
    void onReadyRead() {
        while (process->canReadLine()) {
            QString line = process->readLine().trimmed();
            if (line.isEmpty()) continue;
            
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError) {
                qDebug() << "JSON解析错误:" << error.errorString();
                continue;
            }
            
            QJsonObject obj = doc.object();
            QString type = obj["type"].toString();
            
            if (type == "model_list") {
                emit modelListReceived(obj["models"].toArray());
            } else if (type == "audio_fragment") {
                // 解码十六进制音频数据
                QString hexData = obj["audio_data"].toString();
                QByteArray audioBytes = QByteArray::fromHex(hexData.toUtf8());
                int sr = obj["sampling_rate"].toInt();
                emit audioFragmentReceived(sr, audioBytes);
            } else if (type == "tts_complete") {
                bool success = obj["success"].toBool();
                QString cachePath = obj["cache_path"].toString();
                emit ttsCompleted(success, cachePath);
            } else if (type == "initialized") {
                qDebug() << "TTS服务已就绪";
            }
        }
    }
    
    void onErrorRead() {
        QString errorMsg = process->readAllStandardError();
        qDebug() << "TTS错误:" << errorMsg;
        emit errorOccurred(errorMsg);
    }
    
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus) {
        qDebug() << "TTS服务退出，代码:" << exitCode;
    }
    
private:
    QProcess *process;
};
```

### 4.3 音频播放示例

```cpp
#include <QAudioOutput>
#include <QBuffer>

void playAudioFragment(int samplingRate, const QByteArray& audioData) {
    // 设置音频格式
    QAudioFormat format;
    format.setSampleRate(samplingRate);  // 动态读取，v3=24000，v1/v2=32000
    format.setChannelCount(1);
    format.setSampleSize(16);  // 16位
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    
    // 创建音频输出
    QAudioOutput *audioOutput = new QAudioOutput(format, this);
    
    // 创建缓冲区
    QBuffer *buffer = new QBuffer(this);
    buffer->setData(audioData);
    buffer->open(QIODevice::ReadOnly);
    
    // 播放
    audioOutput->start(buffer);
}
```

### 4.4 HTTP 调用关键片段（推荐方案A）

所有模型版本（含 v1/v2/v3/v3lora/v4）统一通过 HTTP 调用 `api_v2.py`。下方展示关键片段。

#### 4.4.1 启动 api_v2.py 进程

```cpp
// 启动 api_v2.py 作为 HTTP 服务（端口 9880）
QString pythonPath = gptSovitsPath + "/runtime/python.exe";
QString scriptPath = gptSovitsPath + "/api_v2.py";

QStringList args;
args << scriptPath
     << "-a" << "127.0.0.1"
     << "-p" << "9880"
     << "-c" << "GPT_SoVITS/configs/tts_infer.yaml";

process->setProgram(pythonPath);
process->setArguments(args);
process->setWorkingDirectory(gptSovitsPath);  // 关键：工作目录必须是 GPT-SoVITS 项目根
process->start();
```

#### 4.4.2 切换模型

```cpp
// 通过 HTTP GET 调用 /set_sovits_weights
QNetworkRequest request(QUrl("http://127.0.0.1:9880/set_sovits_weights"
                             "?weights_path=SoVITS_weights_v3/千岛茉子.pth"));
// api_v2.py 自动识别模型版本：
// - v1/v2: 走原始 decoder 流程，输出 32000Hz
// - v3: 自动加载 LoRA，走 BigVGAN 流程，输出 24000Hz
// - v4: 走 Generator 流程，输出 48000Hz
networkAccessManager->get(request);
```

#### 4.4.3 调用 /tts 接口

```cpp
// POST /tts，JSON body
QJsonObject body;
body["text"] = "你好，这是测试";
body["text_lang"] = "zh";
body["ref_audio_path"] = "/path/to/ref.wav";
body["prompt_text"] = "参考文本";    // v3/v4 模型必填
body["prompt_lang"] = "zh";
body["text_split_method"] = "cut0";
body["speed_factor"] = 1.0;
body["media_type"] = "wav";
// v3 专用参数（v1/v2 模型会忽略这些参数）
body["sample_steps"] = 32;            // CFM 采样步数
body["super_sampling"] = false;       // 超分辨率

QNetworkRequest request(QUrl("http://127.0.0.1:9880/tts"));
request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
networkAccessManager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
```

#### 4.4.4 处理响应

```cpp
// 响应 Content-Type: audio/wav
// 采样率因模型版本而异：v1/v2=32000Hz，v3=24000Hz，v4=48000Hz
// 解析时：从 wav header 动态读取采样率，不要硬编码

connect(reply, &QIODevice::readyRead, this, [this, reply]() {
    QByteArray chunk = reply->readAll();
    audioBuffer.append(chunk);
    // ... 喂给音频播放器
});
```

> **完整 HTTP 客户端实现（QNetworkAccessManager + 音频播放）属于通用 Qt 代码，可参考 Qt 文档。**
> **关键提示：**
> - v3 模型不支持真正的流式返回（`streaming_mode` 会被回退到分段返回）
> - v3 模型会自动关闭 `parallel_infer` 和 `split_bucket`
> - 建议首次调用时传 `streaming_mode=true`，系统会自动适配

---

## 五、参考音频格式要求

| 要求 | 说明 |
|------|------|
| 格式 | WAV |
| 采样率 | 建议32kHz或更高 |
| 时长 | 3-10秒 |
| 内容 | 清晰的人声，无噪音 |

---

## 六、性能优化建议

1. **使用常驻服务**：避免每次合成都重新加载模型（模型加载约30-60秒）
2. **流式输出**：使用 `return_fragment=True` 实现实时音频输出
3. **音频缓存**：相同文本的合成结果缓存到本地，避免重复合成
4. **批量处理**：长文本使用 `batch_size > 1` 加速合成（v3 模型不支持并行，此项无效）
5. **半精度推理**：GPU支持时使用 `is_half=True`，提升速度并减少显存占用
6. **v3 模型 sample_steps 调优**：v3/v3lora 模型的 `sample_steps`（默认 32）影响 CFM 采样质量和速度。建议范围 16-64：
   - `16`：最快，质量略降（适合实时对话）
   - `32`：默认，平衡质量与速度（推荐）
   - `64`：最慢，质量最佳（适合成品音频生成）
