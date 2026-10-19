# Phase 0 测试报告：架构解耦（大脑 + 渠道）

> 类型：测试报告
> 对应实施：[PHASE0_IMPLEMENTATION.md](PHASE0_IMPLEMENTATION.md)
> 测试日期：2026-10-19
> 测试结论：**通过（自动化 3/3 套件、23/23 用例通过；运行期冒烟 2 项通过，2 项待人工验收）**

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

---

## 五、运行期冒烟测试

使用 Debug 目录下的实际数据副本执行，迁移前已备份为
`app_data/memory/QianDaoMoZi_memory.db.bak_phase0_test`。

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 首次启动触发 v2 迁移，旧记录保留 | ✅ 通过 | 日志：`当前数据库版本: 1` → `正在升级数据库结构至 v2` → `结构版本为: 2` |
| 2 | 二次启动不再迁移（幂等） | ✅ 通过 | 第二次日志仅 `当前数据库版本: 2`，无迁移输出 |
| 3 | 正常对话一轮 | ⏸ 待人工 | 需真实 API/语音环境与 UI 交互 |
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

---

## 六、结论与建议

1. Phase 0 的数据库迁移、双键读写、渠道适配接口与输入路由满足实施报告中的设计预期，存量数据零丢失。
2. 自动化测试已覆盖可离线验证的核心逻辑，可作为后续 Phase 1 改动的回归基线。
3. 后续动作：
   - 人工完成验收项 3、5；
   - 将验收项 4 的表述调整到 Phase 1（或届时在 QQ 渠道装配处显式触发一次身份加载）；
   - 修复项与测试代码需随 Phase 0 一并提交。
