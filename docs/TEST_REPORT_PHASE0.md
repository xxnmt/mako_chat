# Phase 0 测试报告：架构解耦（大脑 + 渠道）

> 类型：测试报告
> 对应实施：[PHASE0_IMPLEMENTATION.md](PHASE0_IMPLEMENTATION.md)
> 测试日期：2026-10-19（自动化 + 冒烟）、2026-10-20（人工异常回复校验 + 缺陷修复验证）
> 测试结论：**通过（自动化 3/3 套件、23/23 用例通过；冒烟 3 项通过、1 项移至 Phase 1、1 项待人工；人工异常回复 5 个场景全部符合预期；发现并修复缺陷 2 项）**

---

## 一、测试环境

| 项 | 值 |
|---|---|
| 系统 | Windows 11 Home China 10.0.26300 |
| 编译器 | MinGW GCC 13.1.0（`D:/QT/Tools/mingw1310_64`） |
| Qt | 6.11.1 mingw_64（Core / Sql / Test） |
| 构建类型 | Debug |
| 构建目录 | `build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug` |
| 测试框架 | Qt Test + CTest |

---

## 二、测试范围

### 已覆盖

| 模块 | 测试点 |
|---|---|
| MemoryManager | v1→v2 迁移、存量数据保留、迁移幂等、新建库结构、渠道/会话双键隔离、meta 入库、摘要分桶、未摘要游标分桶、关系状态 subject 隔离 |
| ConfigManager | identity.json 解析（self/master 账号、盲区、立绘跟随、主动对话配置）、默认值回退 |
| SessionRouter | 请求元数据组装、按会话桶组装历史、空输入拦截、identity 缺失时的身份默认值 |

### 不在自动化范围

- LLM / TTS 网络与外部进程调用（依赖真实 API 与本地 GPT-SoVITS 环境）
- 桌宠 UI 的视觉与交互行为（气泡、立绘、设置界面）
- Phase 1 才启用的 QQ 收发链路（当前为占位实现）

其中异常回复校验与标签兜底不纳入自动化，已由 2026-10-20 人工测试覆盖（见第六节）。

---

## 三、自动化测试结果

运行命令：

```bash
ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

| 套件 | 用例数 | 结果 | 耗时 |
|---|---|---|---|
| `tst_memorymanager` | 10 | 通过 | 0.14 s |
| `tst_configmanager` | 8 | 通过 | 0.03 s |
| `tst_sessionrouter` | 5 | 通过 | 0.04 s |
| **合计** | **23** | **全部通过** | 约 0.2 s |

用例明细：

**tst_memorymanager**

- `legacyMigrationPreservesData` — v1 库迁移后版本为 2，八项结构变更齐全，存量行按默认值归入 `pet/default`，关系值复制到 `subject='user'`，新旧 UNIQUE 约束行为正确
- `migrationIsIdempotent` — 二次构造不重复迁移、不重复写数据
- `freshDatabaseIsV2` — 空库直接建成 v2，默认关系维度 30/30
- `historyBucketsAreIsolated` — 同渠道不同会话、不同渠道之间历史互不可见；管理分页跨渠道
- `metaIsPersistedPerTurn` — channel/session_id/speaker/sensitive 正确入库
- `unsummarizedCursorIsBucketed` — 摘要游标按双键隔离（**修复缺陷后通过**）
- `summaryBucketsAreIsolated` — 长期摘要按双键隔离
- `relationshipSubjectsAreIsolated` — 同一维度可按 subject 独立累计

**tst_configmanager**

- `parsesQQAccountLists` / `masterLookupIsExact` / `blindZoneLooksUpByDoubleKey` / `parsesAppearanceFlag` / `parsesProactiveConfig` / `shortMemoryLengthHasDefault`

**tst_sessionrouter**

- `routesInputWithMetaAndBucketedHistory` / `emptyInputDoesNotEmit` / `identityConfigFallsBackToDefaults`

---

## 四、缺陷发现与修复

### BUG-1（严重）：未摘要历史查询恒为空，长期记忆摘要链路失效

| 项 | 内容 |
|---|---|
| 位置 | `memorymanager.cpp` `MemoryManager::getUnsummarizedTurns` |
| 现象 | 单元测试 `unsummarizedCursorIsBucketed` 期望返回 1 条，实际返回 0 条 |
| 根因 | 代码先 `prepare(sql)` 并 `bindValue()`，随后调用 `query.exec(sql)`。传入 SQL 字符串会使 Qt 重新准备语句并丢弃已绑定的参数，占位符以 NULL 参与比较，`channel = NULL` 恒不成立 |
| 影响 | Phase 0 改动后，记忆提取无法取得任何未摘要对话，长期摘要/关系更新链路实际停止工作 |
| 修复 | 改为 `query.exec()`，复用已绑定的预编译语句 |
| 回归 | `unsummarizedCursorIsBucketed` 通过；全量 23 用例通过 |

### 改进：数据库连接重复打开

`initDatabase` 复用连接名时调用 `QSqlDatabase::database(connName)` 会默认尝试打开旧库文件，旧文件不存在时产生 `unable to open database file` 警告。已改为 `database(connName, false)`，复用时先关闭再按新路径打开。该问题仅在多实例/测试场景出现，不影响单实例运行。

### BUG-2（中等）：空标签被编辑距离误判为枚举首项

| 项 | 内容 |
|---|---|
| 位置 | `llmservice.cpp` `TagValidator::levenshteinDistance` |
| 发现 | 2026-10-20 人工异常回复校验，日志出现 `[LLMTVD]编辑距离修正: "emotion" : "" -> "happyIdle"` |
| 根因 | 空串分支写反：`if(m==0)return m;` / `if(n==0)return n;`，使 `distance("", x)` 恒为 0 |
| 影响 | 缺失标签在编辑距离环节恒命中 `VALID_*` 列表的第一个元素，绕过“继承上一句 → 默认值”的降级链。emotion / blush / distance 的首元素恰好等于默认值，问题被掩盖；clothing 首元素为 `pajama`，缺失时会被错误改为睡衣，且多句轮次不会继承上一句服装，导致同一轮内立绘换装 |
| 修复 | 交换两个分支的返回值（Levenshtein 距离满足 `distance("", x) = len(x)`），空值随后正常进入继承/默认值分支 |
| 回归 | 2026-10-20 22:20 实测：首句缺失 emotion → `使用默认值: "" -> "happyIdle"`；第二句 → `继承上一状态: "" -> "happyIdle"`；数据库 id 150 两句 emotion 均为 `happyIdle` |

---

## 五、运行期冒烟测试

使用 Debug 目录下的实际数据副本执行，迁移前已备份为
`app_data/memory/QianDaoMoZi_memory.db.bak_phase0_test`。

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 首次启动触发 v2 迁移，旧记录保留 | ✅ 通过 | 日志：`当前数据库版本: 1` → `正在升级数据库结构至 v2` → `结构版本为: 2` |
| 2 | 二次启动不再迁移（幂等） | ✅ 通过 | 第二次日志仅 `当前数据库版本: 2`，无迁移输出 |
| 3 | 正常对话一轮 | ✅ 通过 | 2026-10-20 人工对话：`成功保存本轮对话` → 气泡显示 → TTS `本轮对话已全部播放` |
| 4 | identity.json 加载日志 | ⚠️ 预期不符 | Phase 0 无任何生产代码调用 identity getter，懒加载不会在启动时触发；解析逻辑已由单元测试覆盖。建议将该项移至 Phase 1 QQ 渠道接入后验收 |
| 5 | 设置界面历史表格 | ⏸ 待人工 | 跨渠道分页已在单元测试覆盖，UI 显示需人工目检 |

迁移前后数据核对：

| 表 | 迁移前 | 迁移后 |
|---|---|---|
| `chat_history` | 102 行，4 列，user_version=1 | 102 行全部补齐 `channel='pet'`、`session_id='default'` |
| `long_term_summary` | 10 行 | 10 行，全部归入 `pet/default` |
| `episodic_memory` | 4 行 | 4 行，`origin='pet'` |
| `relationship_state` | 2 行（旧结构） | 2 行，`subject='user'`，intimacy 41.5 / trust 35.5 数值不变 |
| `raw_messages` | 不存在 | 已创建，0 行（Phase 0 只建不写） |

补充：2026-10-20 重启验证，应用检测到端口 9880 被占用并清理孤儿 TTS 进程（PID 4856）；数据库版本保持 2，未重复迁移。

---

## 六、人工异常回复校验测试（2026-10-20）

测试方式：在桌宠输入框依次要求模型输出不完整 JSON，观察校验拦截、UI 反馈、TTS 与入库结果，并与数据库交叉核对。

| # | 场景 | 期望 | 结果 | 证据 |
|---|---|---|---|---|
| 1 | 缺 zh_text | 整轮抛弃，不入库、不走 TTS | ✅ | `logs.txt:897` `第 2 句内容异常("zh_text缺失或不含中文")` → `回复内容异常，抛弃本次回复` |
| 2 | 缺 ja_text | 整轮抛弃 | ✅ | `logs.txt:1038` `第 1 句内容异常("ja_text缺失或不含日文假名")` |
| 3 | 缺整个 tags | 整轮抛弃 | ✅ | `logs.txt:1371` `第 1 句内容异常("tags缺失或为空")` |
| 4 | 仅缺 emotion | 兜底为 happyIdle，正常播报并入库 | ✅ | 修复前走编辑距离（见 BUG-2）；修复后：首句 `使用默认值`、次句 `继承上一状态`（2026-10-20 22:20 运行） |
| 5 | 正常对话 | 入库、气泡、TTS 正常 | ✅ | `logs.txt:1197` `成功保存本轮对话 channel="pet" session="default"`，`logs.txt:1235` `本轮对话已全部播放` |

数据库只读核对：

- 截至 21:59，`chat_history` 由 102 行增至 105 行，仅新增 3 个有效轮次（21:32:38、21:41:03、21:58:31）；
- 被拦截的 3 个轮次（缺 zh_text / ja_text / tags）用户输入在库中计数为 0，回复也未写入历史；
- 修复后 22:20:34 的有效轮次 id 150 已入库，共 106 行，两句 `emotion` 均为 `happyIdle`；
- `long_term_summary` 10 行无替换字符；`raw_messages` 仍为 0 行。

补充说明：

1. 21:32:38 首次 zhText 测试时模型未按要求缺字段，被系统提示词纠正，该轮属正常对话；实际拦截由后续更明确的请求触发。
2. 异常回复被拦截时 AppController 会显示道歉气泡（`appcontroller.cpp:162`），但该轮用户输入同样不入库。若需保留用户侧输入，需另行设计。
3. 日志中“最近发生”摘要出现乱码（如 `logs.txt:1057`），经核实为输出捕获在固定缓冲边界拆坏 UTF-8 字符所致：8 次提示词转储的乱码位置均位于以 `[LLM]最终注入的Message` 为起点偏移 5188 字节处，且数据库摘要内容完整、无替换字符，实际请求不受影响。

---

## 七、结论与建议

1. Phase 0 的数据库迁移、双键读写、渠道适配接口与输入路由满足实施报告中的设计预期，存量数据零丢失。
2. 自动化测试已覆盖可离线验证的核心逻辑，可作为后续 Phase 1 改动的回归基线。
3. 人工校验确认异常回复拦截链（zh_text / ja_text / tags / 空回复）与标签兜底链路工作正常：被拦截轮次不入库、不走 TTS，修复后的空标签按“继承 → 默认值”降级。
4. 后续动作：
   - 人工完成验收项 5（设置界面历史表格目检）；
   - 将验收项 4 的表述调整到 Phase 1（或届时在 QQ 渠道装配处显式触发一次身份加载）；
   - 考虑为 `TagValidator` 空标签兜底补充回归用例（需解决 `llmservice.cpp` 的链接依赖）；
   - 修复项与测试代码需随 Phase 0 一并提交。
