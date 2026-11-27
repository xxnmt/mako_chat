# R12-2c-final 基线核对报告

- 核对时间：2026-11-27 23:07–23:16（+08:00，含并发 R12-3a 现场记录）
- 工作目录：`D:\QtProject\Windows_AI`（git 仓库，分支 `main`，远端 `origin = https://github.com/xxnmt/Windows_AI.git`）
- 核对身份：R12-2c-final 基线核对专员（独立只读核对）
- 产出：本文件 `docs/R12_2C_FINAL_BASELINE.md`（新增、未 `git add`，属未跟踪文件）
- 限制：本阶段**未启动 Windows_AI 应用、未发送任何真实 QQ/Pet 消息、未发起任何真实模型请求**；仅执行只读 git 命令、读取构建/运行日志、读取二进制元数据、以只读方式读取 SQLite（`mode=ro`，未写入）。

---

## 0. 结论摘要（TL;DR）

1. **是"只是 SHA 变化、代码树一致"，不是代码回退或功能变化。** 报告中的 `HEAD=a9289c2d2aa373c69d76a921882e55b6e9a1c4b2` 与当前 GitHub/local `main=4dcafb6ffbcf438fde489399bd19315814dbb267` 的**整树 diff 为空**，两棵提交的 `tree` 均为 `c239b5e75228c5760d9321a96a603648216c7fe8`。
2. **发生的是本地提交历史重写（commit 对象重写），不是 rebase 到别的基线、不是代码合并、不是 GitHub 端变化。** 本地 `git reflog` 留下两轮改写动作：先"translate four local commit messages"（→ `bc10393`），再"match repository commit conventions"（→ `4dcafb6`）。四个业务提交的**新旧 tree 逐对完全相同**。旧提交对象仍在对象库中（`git cat-file -t` 均为 commit，受 reflog 保护），可追溯、可恢复。
3. **报告中的 `a9289c2` 在本机记录中从未被推送**：`refs/remotes/origin/main` 的 reflog 在 `1b24195`（22:08:49）之后直接更新为 `4dcafb6`（22:47:57）。按本机记录，报告时刻 GitHub 上的 main 应停在 `1b24195`，报告写的是本地 HEAD。无法从本机排除"其他机器推送过 a9289c2"，但本机无此记录；GitHub 当前 main 经 `git ls-remote` 实测为 `4dcafb6`。
4. **不需要重跑全部真实模型验收**（符合用户给定的豁免条件）。且证据显示最终树确实已跑过真实模型验收：最终测试二进制（22:43:14）在 22:43:32 的全量用例矩阵中 15/15 通过，其中 verifier 调用为真实 `deepseek-v4-flash`、`http=200`；22:43:56 又以 ctest 单独跑通 `tst_memoryagent`（24.57s 通过）。
5. **二进制对应关系可证到"内容级"**：最终二进制内嵌了 `a9289c2` 新增的字面量（`isolated-open-wake-history`、`我今天还醒着，正想聊点轻松的。`），而此前所有版本二进制均**不含**这些字面量。因此"最终二进制由最终测试源码编译"不是猜测，而是可从二进制内容直接验证的事实。仍须诚实声明：**二进制本身不含 commit SHA，无法做密码学上的"A 二进制 = B 提交"证明**，只能做到"内容标记 + 时间线 + 工作区干净 + ctest 指向"的强证据链。
6. **报告原文无法恢复**：`docs/` 从未提交过任何 `R12_2C*` 报告文件，全仓也搜不到该文件；报告只存在于当时的对话输出中，未入库、未落盘、未进备份。可恢复的只有提交/日志/二进制等客观证据，报告的行文与"编号验收矩阵"不可恢复（与用户风险 4 重合）。
7. **一个必须在基线里记录的现场事实（并发 R12-3a 工作区）**：本次核对开始时（23:08 左右）工作区干净（`git status --porcelain` 为空）；核对进行中，工作区出现**并发的 R12-3a 工作**，均**不是本任务产生**：
   - 23:10:06 未跟踪 `src/cognition/cognitivecontext.h`（18,058 B，内容为 "R12-3a 统一认知上下文 shadow 切片"）；
   - 23:12:10 未跟踪 `src/cognition/cognitivecontext.cpp`（105,470 B）；
   - 23:12:42 修改 `src/cognition/llmservice.h`（+16 行）；
   - 23:13:28 修改 `src/cognition/llmservice.cpp`（+295/−3 行量级）；
   - 23:13:32 修改 `CMakeLists.txt` 与 `tests/CMakeLists.txt`；
   - 23:13:21 未跟踪 `docs/R12_3A_SOURCE_INVENTORY.md`（75,409 B）。
   
   （以上为 23:16 成稿时的快照；并发 R12-3a 改动在核对过程中仍在持续增加。）
   
   这些在途改动**未进入任何提交**，HEAD 仍为 `4dcafb6`、`HEAD^{tree}` 仍为 `c239b5e7…`（本文件成稿时复核）。本文件的 Git 结论（§2）基于**提交对象**，不受工作区在途改动影响；但若日后要"从当前工作区重建复验 R12-2c-final"，应先以 `4dcafb6` 的干净检出（或等 R12-3a 落库）为起点，否则构建产物会混入 R12-3a 代码。本任务只新增了 `docs/R12_2C_FINAL_BASELINE.md` 一个文件。

---

## 1. 核对范围与方法

| 项 | 方法 | 是否只读 |
|---|---|---|
| 当前 ref 状态 | `git rev-parse` / `git branch -vv` / `git ls-remote` | 是 |
| 是否历史上被重写 | `git reflog --date=iso`、`git reflog show main`、`git reflog show origin/main` | 是 |
| 旧提交是否还在 | `git cat-file -t`、`git cat-file -e`、`git fsck --unreachable --no-reflogs` | 是 |
| 新旧代码树是否一致 | `git rev-parse '<commit>^{tree}'`、`git diff`、`git diff --stat`、`git diff --name-status` | 是 |
| 提交范围与时间 | `git log --oneline --format`、`git rev-list --count` | 是 |
| 测试二进制来源 | 文件时间戳、SHA-256、二进制内嵌字符串扫描（Python 只读）、构建日志、`.ninja_log`、`CTestTestfile.cmake` | 是 |
| 生产库/配置 | 文件 stat/SHA-256、SQLite `PRAGMA user_version`（`file:...?mode=ro`）、日志 | 是 |

注：PowerShell 下 `git rev-parse HEAD^{tree}` 的 `{...}` 会被 PowerShell 当作脚本块语法，必须加引号。本次统一用单引号形式 `git rev-parse 'HEAD^{tree}'`、`git rev-parse 'a9289c2^{tree}'`，均成功。

---

## 2. Git 考古：只重写了提交信息，代码树未变

### 2.1 当前 ref 状态（全部一致）

```text
$ git rev-parse HEAD            => 4dcafb6ffbcf438fde489399bd19315814dbb267
$ git rev-parse main            => 4dcafb6ffbcf438fde489399bd19315814dbb267
$ git rev-parse origin/main     => 4dcafb6ffbcf438fde489399bd19315814dbb267
$ git rev-parse 'HEAD^{tree}'   => c239b5e75228c5760d9321a96a603648216c7fe8
$ git ls-remote origin refs/heads/main
  4dcafb6ffbcf438fde489399bd19315814dbb267	refs/heads/main      # 实测 GitHub 当前 main
$ git branch -vv
  * main 4dcafb6 [origin/main] test(cognition): R12-2c-final 补充真实模型主动联系验收
$ git rev-list --count HEAD     => 180
$ git rev-list --count origin/main => 180
```

- 本地 `main` == `origin/main` == GitHub 实测 HEAD == `4dcafb6`。
- 全仓 180 个提交，本地与远端数量一致（无未推送提交、无远端多出的提交）。
- 现场状态说明：上述"工作区干净"对应 **23:08 核对开始时刻**。23:10 之后工作区出现并发的 R12-3a 在途改动（未跟踪文件与 `llmservice.*` 的未提交修改，**非本任务**，详见 §0 第 7 条与 §9 第 6 条）。它不改变任何提交对象，也不改变本节结论；但若以"此刻工作区是否全干净"为准，则**此刻不满足**。

提交时间（注意：重写保留了原 author/committer 时间）：

```text
4dcafb6 | 2026-11-27 22:43:42 +0800 | test(cognition): R12-2c-final 补充真实模型主动联系验收
56d1436 | 2026-11-27 22:38:18 +0800 | fix(activity): R12-2c-final-3 分享恢复与同版本单次核验
e5f33b6 | 2026-11-27 22:36:36 +0800 | fix(cognition): R12-2c-final-2/3/4 QQ发送前复验、回顾收尾与记忆恢复
d8d4779 | 2026-11-27 22:19:56 +0800 | fix(action): R12-2c-final-1 补齐证据绑定与核验入口
```

### 2.2 reflog 时间线：三代提交对象，同一棵树

`git reflog --date=iso`（HEAD/main 合并观察）关键片段：

```text
4dcafb6 HEAD@{2026-11-27 22:47:22 +0800}: match repository commit conventions   ← 第 3 代（当前 main）
bc10393 HEAD@{2026-11-27 22:45:52 +0800}: translate four local commit messages   ← 第 2 代
a9289c2 HEAD@{2026-11-27 22:43:42 +0800}: commit: test(cognition): seed an available owner for the live contact scenario  ← 第 1 代，即报告中的 HEAD
e417073 HEAD@{2026-11-27 22:38:18 +0800}: commit: fix(activity): reserve one verifier call per share version
d3b3dca HEAD@{2026-11-27 22:36:36 +0800}: commit: fix(cognition): enforce proactive delivery and recover review state
e4c73ba HEAD@{2026-11-27 22:19:56 +0800}: commit: fix(action): bind the verifier to complete persisted evidence
1b24195 HEAD@{2026-11-27 20:47:38 +0800}: commit: fix(action): R12-2c-final-1 真实verifier预算与内容绑定
```

`git reflog show origin/main --date=iso` 关键片段（证明 a9289c2 从未被本机推送）：

```text
4dcafb6 refs/remotes/origin/main@{2026-11-27 22:47:57 +0800}: update by push
1b24195 refs/remotes/origin/main@{2026-11-27 22:08:49 +0800}: update by push
```

即：`1b24195` 之后远端分支的下一次本机记录是 `4dcafb6`，中间没有任何一次"push 了 a9289c2 / bc10393"的记录。重写动作发生在**本地**，最终形态一次性推送。

### 2.3 逐对 tree hash：四个提交全部一致

四个业务提交（第 1 代 = 报告链；第 3 代 = 当前 main 链）逐对比较：

| 编号 | 第 1 代（报告链） | tree | 第 3 代（当前 main） | tree | 结果 |
|---|---|---|---|---|---|
| R12-2c-final-1 | `e4c73ba4928cec11fb056916414cdaf82535a6ef` | `3f79068072fbcaa24ee982e13d0eae7833d1372c` | `d8d477924cfaf285401c4204f40e0bdcba3284ec` | `3f79068072fbcaa24ee982e13d0eae7833d1372c` | **相同** |
| R12-2c-final-2 | `d3b3dca11b35cba9747188fd2a75bd5342ca3a11` | `e1dd4ae40786cc543c0ffc8f8720556652e53706` | `e5f33b69e54950488f8f29f49b036abffb99bef2` | `e1dd4ae40786cc543c0ffc8f8720556652e53706` | **相同** |
| R12-2c-final-3 | `e417073940666ef27b594981b5a8caabbde56504` | `313a566c67817bbef26a1c7d7b1b9545ef96bae1` | `56d14360cdc6e141e1f27dd2086a22d0cbcef02a` | `313a566c67817bbef26a1c7d7b1b9545ef96bae1` | **相同** |
| R12-2c-final-4 | `a9289c2d2aa373c69d76a921882e55b6e9a1c4b2` | `c239b5e75228c5760d9321a96a603648216c7fe8` | `4dcafb6ffbcf438fde489399bd19315814dbb267` | `c239b5e75228c5760d9321a96a603648216c7fe8` | **相同** |

命令与输出摘要：

```text
$ git rev-parse 'e4c73ba^{tree}' => 3f79068072fbcaa24ee982e13d0eae7833d1372c
$ git rev-parse 'd8d4779^{tree}' => 3f79068072fbcaa24ee982e13d0eae7833d1372c
...
$ git diff --stat e4c73ba d8d4779     => (空)
$ git diff --stat d3b3dca e5f33b6     => (空)
$ git diff --stat e417073 56d1436     => (空)
$ git diff --stat a9289c2 4dcafb6     => (空)
```

**第 2 代中间链也一一对应**（说明第 2 轮改写同样只动提交对象）：

```text
1b24195 (tree b6e27fdc2428bfb50105046c6a9bc531b2c5887c，第 1/2/3 代共同基线)
  └─ 5466e82  tree 3f79068072fbcaa24ee982e13d0eae7833d1372c  = e4c73ba 的树
      └─ 6c7bbe  tree e1dd4ae40786cc543c0ffc8f8720556652e53706  = d3b3dca 的树
          └─ fb429ce  tree 313a566c67817bbef26a1c7d7b1b9545ef96bae1  = e417073 的树
              └─ bc10393  tree c239b5e75228c5760d9321a96a603648216c7fe8  = a9289c2 的树
```

### 2.4 整树 diff 为空（最终判据）

```text
$ git diff --stat a9289c2 4dcafb6          => (空，输出无任何行)
$ git diff --name-status a9289c2 4dcafb6   => (空，输出无任何行)
```

`git diff` 对内容做逐 blob 比较；输出为空即整棵树（全部文件内容与权限）完全一致。结合 `tree` hash 相同（`c239b5e7…`），这是"代码树一致"的**最强本地判据**。

### 2.5 旧提交是否仍存在（可追溯性）

```text
$ git cat-file -t e4c73ba  => commit
$ git cat-file -t d3b3dca  => commit
$ git cat-file -t e417073  => commit
$ git cat-file -t a9289c2  => commit
$ git cat-file -t bc10393  => commit
$ git cat-file -e a9289c2  => exit 0（对象存在）
$ git branch -a --contains a9289c2  => (无输出；不在任何分支上)
$ git merge-base a9289c2 4dcafb6    => 1b24195495b7e6daf86a8237fcfdddf1c85a9d32
$ git fsck --unreachable --no-reflogs
  unreachable commit e4c73ba… d3b3dca… e417073… a9289c2…   （第 1 代 4 个）
  unreachable commit 5466e82… 6c7bbe… fb429ce… bc10393…    （第 2 代 4 个）
  unreachable commit 3371d5e9… 017b73ca…                    （与本任务无关的 2026-11-03/04 旧悬空提交）
```

结论：`a9289c2` 等 8 个被重写的提交**仍然完整存在于本地对象库**（`git cat-file` 可读、reflog 引用保护），历史可追溯；两代重写的 tree 与当前树完全可对账。

### 2.6 四提交合计改了什么（供基线存档）

```text
$ git log --oneline 1b24195..4dcafb6   => 4 个提交（d8d4779 / e5f33b6 / 56d1436 / 4dcafb6）
$ git diff --stat 1b24195 4dcafb6
  22 files changed, 894 insertions(+), 159 deletions(-)
  涉及：docs/R12_OPEN_WAKE.md、src/app/{actionoutboxdispatcher,appcontroller,cognitivecoordinator,privatereview}.cpp、
        src/channels/qq/{qqchanneladapter.*,qqprotocolclient.*}、src/cognition/{actionpolicy.*,llmservice.cpp}、
        src/memory/{memorymanager.*,memoryretrieval*.cpp,privatereflection.cpp}、
        tests/{CMakeLists.txt,testactionverification.h,tst_actionoutbox.cpp,tst_memoryagent.cpp,tst_memorymanager.cpp}
```

### 2.7 结论（Git 部分）

- **性质**：本地 commit 对象重写（rebase/重放类操作，两轮），仅改**提交信息**（英文 → 中文 → 带 `R12-2c-final-N` 编号的中文规范格式）；author/committer 时间被保留；**代码 tree 全程未变**。
- **不是**：rebase 到新基线、代码合并/回退、GitHub 端变更、内容冲突修复。
- **报告为何对不上**：报告写的是本地第 1 代 HEAD `a9289c2`（从未推送）；GitHub 当前是第 3 代 `4dcafb6`（tree 相同）。

---

## 3. 测试二进制与最终报告的对应关系

### 3.1 现场清单（build/stage-b）

| 二进制/目录 | 文件时间（mtime） | 大小 | SHA-256 前 16 位 | 用途（据日志/目录名） |
|---|---|---|---|---|
| `build/stage-b/Windows_AI.exe` | 2026-11-27 22:38:06 | 109,710,968 | `98C51A224CD57E72` | 当前 stage-b 应用二进制 |
| `build/stage-b/tests/memory-agent/tst_memoryagent.exe` | 2026-11-27 22:43:14 | 61,632,397 | `F44A63A56E54294E` | **最终** `tst_memoryagent`（ctest 测试 #9 指向它） |
| `r12-acceptance-final/tst_memoryagent.exe` | 22:43:14 | 61,632,397 | `F44A63A56E54294E` | 与上一行**同一内容**（最终验收矩阵用副本） |
| `r12-acceptance-final/migration/v32/Windows_AI.exe`、`v33/Windows_AI.exe` | 22:38:06 | 109,710,968 | `98C51A224CD57E72` | 迁移/窗口验收用应用二进制（与 stage-b 根 exe 同内容） |
| `r12-e417073/tst_memoryagent.exe` | 22:38:09 | 61,631,305 | `1F53C49789570C64` | 中间树（e417073）复跑 |
| `r12-controlled/tst_memoryagent.exe` | 22:40:34 | 61,630,861 | `B6014EC360101A98` | 对照场景 |
| `r12-real-final/tst_memoryagent.exe` | 22:33:44 | 61,629,257 | `272A7F13BC924BAD` | 真实模型复跑（中间树） |
| `r12-real/tst_memoryagent.exe` | 22:30:51 | 61,386,609 | `BA5EF509C7DCDEBF` | 首次真实模型场景（中间树） |
| `r12-recovery-tests/{tst_actionoutbox,tst_memorymanager}.exe` | 22:35:17 / 22:35:24 | — | — | 恢复/对账用例 |
| `r12-share-restart/tst_actionoutbox.exe` | 22:38:02 | — | — | 分享重启用例 |
| `r12-db-recovery/{v32,v33}/Windows_AI.exe` | 22:30:49 | 109,487,196 | `655E75C4FBF0AC6A` | 库恢复用例（**另一份**应用二进制） |
| `r12-acceptance-final/migration/{v32,v33}/app_data/memory/*.db` | 22:44:00 / 22:44:01 | 见 §6 | — | 迁移验收用的生产库副本 |

> 说明：任务清单里提到的 `r12-real-history-final` **不是目录**，只是 `build/stage-b/r12-real-history-final-build.log`（22:43:14，213 字节）与 `r12-real-history-build.log`（22:42:21）；它们记录的重建输出指向 `build/stage-b/tests/memory-agent/tst_memoryagent.exe`，即上表 22:43:14 的最终二进制。场景名（real-history）未单独建目录，是复用了最终二进制并追加 `app_data` 运行。

### 3.2 内容级标记：二进制到底由哪棵树编译（关键证据）

测试源码里几个可扫描字面量，分别由不同提交引入（`git log -S` 可查）：

| 标记 | 引入提交（第1代 / 第3代） | 说明 |
|---|---|---|
| `openWakePersistConflictDoesNotPromoteOtherContent` | 基线 `1b24195`（`1b24195`/`1b24195`） | 更早就有 |
| `actionEvidenceBinding` | `e4c73ba` / `d8d4779` | R12-2c-final-1 引入 |
| `realVerifierRejectsUnsupportedClaim` | `d3b3dca` / `e5f33b6` | R12-2c-final-2 引入 |
| `isolated-open-wake-history`、`我今天还醒着，正想聊点轻松的。` | `a9289c2` / `4dcafb6` | **最后一个提交**引入 |

用 Python 对二进制做原始字节扫描（`QStringLiteral` 以 UTF-16LE 存储，普通字符串字面量以 ASCII/本地编码存储；两种编码都扫）：

| 二进制 | `actionEvidenceBinding` | `realVerifierRejectsUnsupportedClaim` | `isolated-open-wake-history`(UTF-16) | `我今天还醒着…`(UTF-16) |
|---|---|---|---|---|
| 最终 `tests/memory-agent/tst_memoryagent.exe`（22:43:14） | PRESENT | PRESENT | **PRESENT** | **PRESENT** |
| `r12-e417073`（22:38:09） | PRESENT | PRESENT | absent | absent |
| `r12-control`（22:40:34） | PRESENT | PRESENT | absent | absent |
| `r12-real-final`（22:33:44） | PRESENT | PRESENT | absent | absent |
| `r12-real`（22:30:51） | PRESENT | **absent** | absent | absent |
| Desktop Debug 的 `tst_memoryagent.exe`（21:21:06） | **absent** | absent | absent | absent |

由此可**从二进制内容反推源码树**（结合提交时间线）：

| 二进制 | 可反推的源码树（tree） | 对应提交（第 3 代） |
|---|---|---|
| Desktop Debug 21:21 构建 | 早于 `e4c73ba`（≈ `1b24195` 时代树 `b6e27fd…`） | 早于 R12-2c-final-1 |
| `r12-real` 22:30:51 | `3f79068…` | `e4c73ba` / `d8d4779` |
| `r12-real-final` 22:33:44 | `e1dd4ae…`（提交前的工作树，晚 3 分钟提交） | `d3b3dca` / `e5f33b6` |
| `r12-e417073` 22:38:09、`r12-controlled` 22:40:34 | `313a566…`（提交前的工作树） | `e417073` / `56d1436` |
| **最终二进制 22:43:14** | **`c239b5e…`** | **`a9289c2` / `4dcafb6`** |

这条链非常整齐：每个二进制都在对应提交前几十秒~几分钟从"提交前工作树"构建（先构建、后提交），且内容标记与 tree 变化严格对应。**最终二进制包含最后一个提交新增的字面量，因此它不是旧二进制的改名/复制，而是确实由最终源码编译。**

### 3.3 构建/测试日志（哪棵树、跑了什么）

关键日志与含义（根目录 `build/stage-b`）：

| 日志 | 时间 | 内容摘要 | 对应树/结论 |
|---|---|---|---|
| `r12-build.log` | 22:24:53 | 202/202，含 `Windows_AI.exe` 与全部测试 exe | e4c73ba 后的一轮全量构建 |
| `r12-final-build.log` | 22:28:24 | 117/117 全量链接 | 中间树 |
| `r12-acceptance-build.log` | 22:30:52 | 全量链接 144 步（首行 `[0/1] Re-running CMake...`，末行 `[143/144]` 链接 `tst_memoryagent.exe`） | 中间树（d3b3dca 前） |
| `r12-verified-build.log` | 22:33:44 | 43 步（末行 `[43/43]`），编译 `actionpolicy.cpp`、`privatereview.cpp`、`memorymanager.cpp`、`tst_memoryagent.cpp`，并链接 `Windows_AI.exe`/`tst_memoryagent.exe` | d3b3dca 前工作树 |
| `r12-at-most-once-build.log` | 22:38:09 | 32/32 完成（日志首行 `[1/41]`），编译 `privatereview.cpp`、`actionpolicy.cpp` 等 | e417073 前工作树 |
| `r12-real-history-build.log` | 22:42:21 | 3 步：重建 `tst_memoryagent.cpp.obj` + 链接 | 最终测试源码（a9289c2 前） |
| `r12-real-history-final-build.log` | 22:43:14 | 同上 3 步 | **最终测试源码 → 22:43:14 最终二进制** |
| `r12-final-ctest.log` | 22:29:05 | 23 项：21 通过，`tst_memorymanager`、`tst_configmanager` **失败** | 中间树（后修复） |
| `r12-ctest.log` | 22:25:33 | 23 项：22 通过，`tst_memorymanager` 失败 | 中间树 |
| `r12-acceptance-ctest.log` | 22:32:51 | **100%：23/23 通过**，118.42s | 中间树 |
| `r12-verified-ctest.log` | 22:35:42 | **100%：23/23 通过**，118.09s | 中间树 |
| `r12-at-most-once-ctest.log` | 22:40:07 | **100%：23/23 通过**，118.64s | `313a566`（= 最终树去掉最后一个"仅测试文件"改动） |
| `r12-final-memoryagent-ctest.log` | 22:41:32 | ctest #9 `tst_memoryagent` 通过，26.36s | 中间/最终交界 |
| `r12-last-memoryagent-ctest.log` | 22:43:56 | ctest #9 `tst_memoryagent` 通过，24.57s | **最终二进制（22:43:14）** |
| `r12-acceptance-final/r12-matrix.log` | 22:43:32 | `TestMemoryAgent` 全量 15/15 通过；含真实 `provider=deepseek model=deepseek-v4-flash http=200`；`Totals: 15 passed, 0 failed … 18043ms` | **最终二进制、真实模型验收** |
| `r12-e417073/r12-matrix.log` | 22:38:49 | 14/15，`realActionAcceptance(A-contact)` 失败（`tst_memoryagent.cpp:3295`，`action_outbox` 计数 0≠1） | 中间树，最后提交修复前 |
| `r12-e417073/r12-contact-repeat-1.log` | 22:39:22 | 14/15（`completion.at(3).isEmpty()` 失败，日志标注 `(truncated)`） | 中间树 |
| `r12-e417073/r12-contact-repeat-2.log` | 22:39:29 | 14/15（`action_outbox` 计数 0≠1） | 中间树 |
| `r12-controlled/r12-matrix.log` | 22:41:09 | 14/15，同一 `A-contact` 失败 | 中间树 |
| `r12-share-restart/r12-share-restart.log` | 22:38:13 | 3/3 通过（分享恢复 + 重启 + 同版本单次核验） | 中间树 |

补充：`build/stage-b/tests/CTestTestfile.cmake` 中测试 #9 定义为
`add_test([=[tst_memoryagent]=] "D:/QtProject/Windows_AI/build/stage-b/tests/memory-agent/tst_memoryagent.exe")`，
即 **22:43:56 的最终 ctest 运行的正是 22:43:14 那个二进制**。

**中间失败不是"最终树失败"**：`r12-e417073`/`r12-controlled` 里 `realActionAcceptance(A-contact)` 的失败（`action_outbox` 计数、截断断言）恰好是最后一个提交 `a9289c2` 修复的对象——该提交在 `tests/tst_memoryagent.cpp` 中 +13/−2 行，为联系人场景预置"主人可用"状态并断言 `wake.kind`。最终二进制里真实模型矩阵 15/15 通过，覆盖的正是这个场景。

### 3.4 能证明到什么程度（不夸大）

**能证明的：**
1. `git diff a9289c2 4dcafb6` 为空 + 两提交 tree 相同 → 报告树与当前树**逐字节一致**（git 对象级，可重复验证）。
2. 最终测试二进制（22:43:14）内嵌了最后提交新增的字面量，而先前二进制不含 → 最终二进制**确实由最终测试源码编译**（内容级）。
3. 最终 ctest/matrix 日志（22:43:32、22:43:56）与最终二进制在**同一目录、同一 ctest 注册路径**、时间在二进制之后 → 最终验收跑的是最终二进制。
4. 核对开始时刻（23:08）复核：自 22:42:13（最后一次源码修改）起，已跟踪文件无改动（`git diff HEAD` 为空）；最后提交时间 22:43:42 > 二进制 22:43:14；重写动作（22:45:52、22:47:22）只改提交信息（§2 已证）。23:10 之后的并发 R12-3a 改动不在上述窗口内、与本轮验收无关（见 §9 第 6 条）。

**不能证明的（如实标注）：**
1. 二进制**不含 commit SHA**，无法做密码学上的"该 exe 唯一对应 `4dcafb6`"证明；上述是"内容标记 + 时间戳 + tree 一致 + ctest 路径"的强证据链，而非形式化证明。
2. 无法从二进制反推构建时的**未提交工作树**是否与提交树完全一致；只能说：源码最后修改 22:42:13 < 二进制 22:43:14 < 提交 22:43:42，且该提交 diff 恰为测试文件的 +13/−2 行、与二进制内嵌的新字面量吻合。
3. 中间各二进制的 tree 归属是**按内容标记阶梯推断**的（每个标记的引入提交已知、各二进制标记组合唯一），不是直接读取的构建元数据。
4. 生产环境（Desktop Debug，21:21 构建）**早于** R12-2c-final 四个提交，含最终修复与否**不能**从该二进制内容证明；本轮按要求未重建、未启动。

### 3.5 两套 build 目录的时间/用途区别

| 维度 | `build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug` | `build/stage-b` |
|---|---|---|
| 生成器/构建方式 | Qt Creator（CMake + 每目标 `objects.a` 静态归档） | CMake + Ninja（`.ninja_log` 可逐对象追踪） |
| Qt/工具链 | Qt 6.11.1 MinGW 64-bit，Debug | 同左，Debug（运行日志 `build=debug`） |
| `Windows_AI.exe` | 21:21:52，109,340,238 B，SHA `3356CA6D…` | 22:38:06，109,710,968 B，SHA `98C51A22…` |
| 测试 exe | 21:19:45–21:21:06 一批 | 22:30–22:43 多批（见 §3.1） |
| app_data | **生产配置**：真实 `identity.json`（1,425 B，2026-11-27 00:05:57）、`setting.json`（3,408 B，11-22 18:28:36）、v33 生产库 110,641,152 B | 各 `r12-*/app_data` 为**测试专用**副本（无 `identity.json`，QQ 关闭；日志明示"identity.json 不存在，使用默认配置"） |
| 用途 | 用户日常运行（生产环境基线，见 §6） | R12 验收/矩阵/迁移/恢复实验，不承载真实身份 |
| 与最终树关系 | **早于** R12-2c-final-1（21:21 < 22:19），不含四个修复 | 应用 exe 22:38:06 建于 `313a566`；最终树只在其上改了 `tests/tst_memoryagent.cpp`，故该应用二进制与最终树的 **src 部分等价** |

---

## 4. 结论：是否满足"只是 SHA 变化、代码树一致"；是否重跑真实验收

### 4.1 是否满足条件

**满足，且是"仅提交信息重写"这一子类。**

判据（全部满足）：
- `git diff a9289c2 4dcafb6` 为空、`--name-status` 为空；
- `a9289c2^{tree} == 4dcafb6^{tree} == c239b5e75228c5760d9321a96a603648216c7fe8`；
- 四个业务提交逐对 tree 相同（§2.3）；
- 第 2 代中间链也逐对 tree 相同（§2.3 末）；
- 重写动作只出现在本地 reflog；提交信息确有两轮改写（英文→中文→规范中文），author/committer 时间保留。

### 4.2 是否需要重跑真实模型验收

**不需要**（用户已明确豁免；且证据已覆盖）：

1. 代码树逐字节一致 → 重跑真实模型验收在本任务定义下不带来新的代码结论。
2. 最终树**确实已执行过**真实模型验收：`r12-acceptance-final/r12-matrix.log`（22:43:32）中 `TestMemoryAgent` 15/15 通过，包含多次真实 `deepseek-v4-flash` 调用（`http=200`、`finish_reason=stop`、`latency_ms≈1700–2600`）；使用的正是最终二进制（§3.2/§3.3）。
3. 最终 `tst_memoryagent` 在 22:43:56 以 ctest 再跑一次通过（24.57s）。

**但须如实说明两点**：真实模型结果本身**有随机性**，历史通过不等于未来每次都过（正是遗留风险 1 的来源）；本次核对**未**重放真实调用。

### 4.3 日后可在树上直接复验的项（本轮均未执行，仅列出）

| 可复验项 | 方式 | 是否需要网络/凭据 |
|---|---|---|
| 代码树一致性 | `git diff a9289c2 4dcafb6`、`git rev-parse '<c>^{tree}'` | 否 |
| 离线测试 23 项 | 在 `4dcafb6` 上重新构建后 `ctest`（除 `tst_memoryagent` 的真实模型用例与需要真实配置的用例外） | 否 |
| 对账/恢复/迁移/分享/Outbox 专项 | `tst_actionoutbox`、`tst_memorymanager`、`tst_qqdelivery` 等 | 否 |
| 真实模型用例 | `tst_memoryagent` 的 `realActionAcceptance`、`realVerifierRejectsUnsupportedClaim` | **是**（网络 + API 凭据） |
| 二进制↔源码对应 | §3.2 的字面量扫描法（Python 只读） | 否 |

注意：若日后在**当前工作区**重建，会混入 23:10–23:13 出现的并发 R12-3a 在途改动（未跟踪 `src/cognition/cognitivecontext.{h,cpp}`、未提交的 `src/cognition/llmservice.{h,cpp}` 修改，均非本任务，见 §9 第 6 条）；如只复验 R12-2c-final，应先取得 `4dcafb6` 的干净检出。

---

## 5. R12-2 遗留风险登记（原文保留，记录不扩大开发）

> 以下 5 项**原文保留**用户给定表述，仅补充客观证据、影响与"后续处理条件"。本阶段**只登记，不扩大开发**。

### 5.1 主模型偶发生成截断

- **原文**：主模型偶发生成截断。
- **证据**：
  - 中间树日志直接出现过截断断言失败：`r12-e417073/r12-contact-repeat-1.log`（22:39:22）中
    `FAIL! : TestMemoryAgent::realActionAcceptance(A-contact) 'completion.at(3).toString().isEmpty()' returned FALSE. (truncated)`（`tests/tst_memoryagent.cpp(3284)`）。
  - 真实 verifier 日志字段显示 `max_tokens=1024`，实际 `output_chars` 约 176–193、`finish_reason=stop`（本次未出现 `finish_reason=length`）。
  - 最终四提交里 `src/cognition/llmservice.cpp` 有 37 行改动（`git diff --stat 1b24195 4dcafb6`），与输出/截断处理相关。
- **影响**：真实模型返回被截断时，用例断言或语义核验可能拿到不完整文本；当前最终树通过（15/15），但这是概率性风险，不是已消除的确定性保证。
- **后续处理条件**：若后续观察到 `finish_reason=length` 或截断断言失败，再单独定位（是否需要在 `llmservice` 层做长度预检/重试策略）；本阶段不扩大。

### 5.2 极端情况下异常回执记录阻塞对账

- **原文**：极端情况下异常回执记录阻塞对账。
- **证据**：
  - 对账/Outbox 恢复路径已实装且被验证：`r12-share-restart/r12-share-restart.log`（22:38:13，3/3 通过）出现
    `[Activity] share_intent_invalidated ref=activity/2 artifact_version=2 reason=verification_outcome_unknown`
    与 `[Receipt] key=activity:1:2:share outcome=protocol_accepted history=projected`。
  - 数据库停机日志（`r12-acceptance-final/migration/v33/app_data/logs/logs_20261127_224401_078…txt`）中，
    对账统计为 `by_outcome {needs_evidence:31, no_change:26, updated:220}`, `pending:0`, `oldest_due_epoch:0`。
- **影响**：正常路径 `pending=0`；但"异常回执 + 未知结果"的极端组合仍可能让个别记录停在待处理/需补证状态，阻塞对账推进（当前无现场故障证据，属设计边界登记）。
- **后续处理条件**：若生产日志出现 `pending>0` 持续不降、或 `verification_outcome_unknown` 反复出现，再专项排查重试/超时/人工兜底；本阶段不动代码。

### 5.3 证据文本摘要与完整来源版本身份的区别

- **原文**：证据文本摘要与完整来源版本身份的区别。
- **证据**：
  - 语义验证日志同时打印摘要与字节数：`[Semantic] action request … digest=632fea7a3e25c048 bytes=1059 claims=1/0 sources=1/0`，随后 `action verifier … digest=…`。
  - 测试 `actionEvidenceBinding` 覆盖"完整持久化证据绑定"（8 个数据行：normal / near-budget / different-sql-scan-order / over-budget / changed-during-verification / changed-before-commit / changed-after-commit / missing-during-verification），最终 15/15 通过。
  - `src/cognition/actionpolicy.cpp`、`src/app/actionoutboxdispatcher.cpp` 与 `tests/testactionverification.h`（+59 行）为证据绑定/核验入口相关实现。
- **影响**：`digest` 是摘要，能证明"核验过某段文本"并防篡改，但**不等于**"完整来源版本"的身份/可复现性；后续若要审计"当时到底核验了哪一完整版本"，仅有摘要可能不足。
- **后续处理条件**：若出现审计/追溯需求（需要完整来源快照身份），再评估是否补充版本标识/快照持久化；本阶段只登记这一语义边界。

### 5.4 原编号验收矩阵未完整保留

- **原文**：原编号验收矩阵未完整保留。
- **证据**：
  - 核对开始时 `docs/` 有 18 个文件，**没有** `R12_2C*` 报告（核对期间另出现并发新增的 `docs/R12_3A_SOURCE_INVENTORY.md`，非本任务，见 §7）；
  - `git log --all --diff-filter=A --name-only -- docs` 中从未出现过 `R12_2C*`/`*FINAL*`（仅历史上有 `docs/R12_OPEN_WAKE.md`、`docs/MEMORY_V5_R3_BASELINE.md`）；
  - 全工作区按文件名搜 `R12_2C*` 无结果；报告本身未入库（详见 §7）。
  - 可替代的**原始**证据只有：`r12-*-ctest.log`、`build/stage-b/*/r12-matrix.log`、构建日志（均保留，见 §3.3）。
- **影响**：报告的"编号验收矩阵"（编号 ↔ 结果 ↔ 证据的对照）不可复现原文；本文件与现有日志只能恢复"跑过什么、结果如何"，无法恢复原始编号编排与逐条措辞。
- **后续处理条件**：本阶段不要求重建原矩阵；若 R12-3 需要正式验收矩阵，建议新建带编号的验收表并入库（如 `docs/R12_3_ACCEPTANCE.md`），以本文件 §3.3 的日志为证据源。

### 5.5 实际 QQ/Pet 自然投递尚需后续运行观察

- **原文**：实际 QQ/Pet 自然投递尚需后续运行观察。
- **证据**：
  - 生产配置中 QQ 渠道为启用状态、存在真实 `identity.json`（见 §6，敏感值不在此记录）；主动联系目标为 `qq_private`。
  - 但本轮**未启动应用、未发真实消息**；测试日志中的投递只覆盖 Pet 内部通道与 Outbox 交接，例如
    `r12-share-restart.log`：`[Outbox] settled … outbox=dispatched`、`raw_events写入: platform="pet" channel="pet"`，
    以及矩阵日志中的 `[Outbox] settled key=activity:1:3:share … outcome=handed_off`。
  - `r12-real-model.log`（22:31:04）的真实模型用例也只到 `handed_off`/`dispatched`，未验证 QQ 端"对方实际收到"。
- **影响**：R12-2c-final 的"投递链路"在内部/协议层已验证；**真实 QQ/Pet 自然投递**（端到端、含对方真实接收与回执）仍需在用户正常使用中观察，不属于可离线复验项。
- **后续处理条件**：用户正常启动应用后自然观察即可；如出现投递失败/重复/回执异常，再按 §5.2 的对账路径排查。本阶段不为此强制启动应用，也不为 R12-3 强制完成整天日志测试。

---

## 6. 生产环境基线（只读核对，未启动、未修改、未迁移）

**生产运行目录**：`build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug`（判断依据：该目录 `app_data` 下同时具备真实 `identity.json`、真实 QQ 配置、110 MB 级 v33 数据库；`build/stage-b/app_data` 则是 2026-11-21 的 319 KB 小库、无 `identity.json`、QQ 关闭，属测试环境，不是生产）。

### 6.1 数据库：保持 v33，不重新迁移、不清理历史

```text
# 只读打开（file:...?mode=ro），未写入
$ python (sqlite3, mode=ro)
  PRAGMA user_version = 33
  table_count = 41
```

文件状态（核对前后一致）：

| 文件 | mtime | 大小 | SHA-256 |
|---|---|---|---|
| `Desktop…Debug/app_data/memory/QianDaoMoZi_memory.db`（生产） | 2026-11-27 19:38:49 | 110,641,152 | `8628AD01EA653B48B28DB6FE8EEEAD639AB9BAA69585B8EB0D221F2EEA020F19` |

- 生产库自 **19:38:49** 后未被本阶段触碰（未打开写、无 `-wal/-shm` 残留；读取用 `mode=ro`）。
- 迁移验收使用的是**生产库的副本**，与生产文件 SHA-256 完全相同（同一内容）：
  - `r12-acceptance-final/migration/v33/…/QianDaoMoZi_memory.db` → `8628AD01…`（110,641,152 B，mtime 19:38:49）
  - `r12-db-recovery/v33/…/QianDaoMoZi_memory.db` → `8628AD01…`（同上）
  - `r12-acceptance-final/migration/v32/…` 与 `r12-db-recovery/v32/…` 为 107,401,216 B 的 v32 副本（22:44:00 / 22:31:55），迁移日志显示其被**在副本内**升级到 33（`v32` 日志：`当前数据库版本: 32` → `数据库结构升级完成,版本: 33`）；生产库本身未参与该升级。
- 因此：**"继续 v33、不重新迁移、不清理历史"成立**（生产库版本 33，历史备份全部保留）。

### 6.2 配置：identity.json / setting.json 未变

| 文件 | mtime | 大小 | SHA-256 前 16 位 |
|---|---|---|---|
| `Desktop…Debug/app_data/config/identity.json` | 2026-11-27 00:05:57 | 1,425 | `0E8EFAA3B588B555` |
| `Desktop…Debug/app_data/config/setting.json` | 2026-11-22 18:28:36 | 3,408 | `91C90C2211BC7809` |

- `identity.json` 顶层键：`appearance / identity / proactive / cognition`。
- **`contact_daily_limit = 0` 已确认成立**：该字段位于 `identity.json` 的 `proactive` 段（不在 `setting.json`）。
  `proactive` 其余关键项（非敏感）：`enabled=true`、`daily_limit=288`、`cooldown_seconds=60`、`idle_seconds=120`、
  `concern_default_check_minutes=30`、`quiet_hours={start:"23:59", end:"00:00"}`、`targets=[{channel:"qq_private", session_id:"2014366825"}]`。
- `setting.json` 顶层键：`api / conversation / memory / qq / time / tts`；QQ 渠道 `enabled=true`（访问令牌按安全要求**不记录**于本文件）。
- 本阶段**未修改**上述任何文件。

### 6.3 备份与可运行版本：保留现状

- 生产库目录保留有历史备份（示例，均未改动）：
  `QianDaoMoZi_memory.db.bak_r11_20261125_234653`（86,745,088 B）、
  `QianDaoMoZi_memory.db.bak_b5_baseline_20261125_220815`（86,347,776 B）、
  `QianDaoMoZi_memory.db.bak_before_v30_20261125_004447`、
  `QianDaoMoZi_memory.db.bak_before_v25/v26/v27/v28_*` 等；配置另存有
  `identity.json.bak_natural_20261127_000555`、`setting.json.bak_*` 多份。
- **可运行版本保留**：`Desktop…Debug/Windows_AI.exe`（21:21:52，109,340,238 B，SHA `3356CA6D…`）原样保留，**未重建、未替换**。
  - 如实说明：该 exe **早于** R12-2c-final 四个提交（22:19–22:43），因此它**不包含**本轮四个修复；如需在生产中体验最终修复，需另行用同一份 `app_data` 构建（**本阶段未执行**，也不在 R12-3 的强制范围内）。
  - `build/stage-b/Windows_AI.exe`（22:38:06）是 R12 验收用的较新应用二进制；其源码树与最终树仅差测试文件，如需生产可另行评估，但本轮不做替换。

### 6.4 本阶段的边界（已遵守）

- 未启动 Windows_AI 应用（未运行 `build/.../Windows_AI.exe`，也未运行测试 exe；所有测试结论均来自既有日志）。
- 未发送任何真实 QQ/Pet 消息。
- 未发起任何真实模型请求（唯一一次 SQLite 是只读 `mode=ro`）。
- 未重新迁移数据库、未清理历史、未删除/移动任何备份、未改 `src/tests/build`（工作区中出现的 `src` 改动为并发的 R12-3a 工作，非本任务，见 §9 第 6 条）。
- **用户可自行启动正常使用**；R12-3 不需要强制完成"整天日志测试"，自然使用观察即可（对应 §5.5）。

### 6.5 生产环境相关遗留提示

1. 若用户直接用 `Desktop…Debug/Windows_AI.exe` 启动，运行的是 R12-2c-final 之前的代码；本轮核对**不改变**这一点（按要求不动生产版本）。
2. 生产库 v33 与配置未动；下一次正常启动不会触发迁移（已是 33），也不会清理历史。
3. 生产配置含真实凭据，本文件刻意未记录令牌等敏感值；共享/归档本文件不构成凭据泄露。

---

## 7. "报告本身未入库、无法恢复原文"的事实与影响（用户已知风险 4）

**事实（已核实）：**

1. 核对开始时 `docs/` 有 18 个文件（`R12_OPEN_WAKE.md`、`AGENT_WORKLOG.md`、`B_STAGE_TASKS.md`、`STATUS.md`、`B_STAGE_ACCEPTANCE.md`、`ROADMAP.md`、`ARCHITECTURE.md`、`CONFIG_REFERENCE.md`、`README.md` 等），**没有任何** `R12_2C*` 或最终报告文件。核对期间又出现并发新增的未跟踪 `docs/R12_3A_SOURCE_INVENTORY.md`（23:13:21，R12-3a 在途，非本任务），以及本文件。
2. `git log --all --diff-filter=A --name-only -- docs` 历史中，`docs` 下新增过的 R12 相关文件只有 `docs/R12_OPEN_WAKE.md`，从未出现过 `R12_2C*`/`*FINAL*` 报告。
3. 全工作区按文件名 `R12_2C*` 搜索结果为空；`build/` 下也没有报告副本。
4. 因此报告**只可能存在于当时的对话输出**，没有落盘、没有入库、没有备份可恢复。

**影响：**

- 报告中的**行文、结论措辞、编号验收矩阵、逐条证据引用**不可恢复（与 §5.4 风险重合）。
- 但报告的**事实性内容**与当前基线的关系可由本文件恢复：代码树一致（§2）、二进制/日志对应（§3）、生产基线（§6）均有客观证据。
- 本文件 `docs/R12_2C_FINAL_BASELINE.md` 是**新增文件且尚未 `git add`**；是否入库由用户决定。若希望此后可追溯，建议将本文件（以及 §5.4 建议的新验收矩阵）提交入库。

**无法恢复项清单（明确）：**

1. 上一轮最终报告原文（含编号验收矩阵与措辞）。
2. 报告写作时刻的"逐项证据引用"原始对应关系（只能由 §3 的日志重建近似）。
3. 报告生成前的本地工作区瞬时状态（未提交中间态；只能由时间线与构建日志旁证）。
4. 报告对应的远端状态（本机无 a9289c2 推送记录，无法确认是否曾有其他机器推送过该 SHA）。

---

## 8. 证据索引（可复跑命令 / 文件）

### 8.1 Git（全部只读）

```powershell
git rev-parse HEAD; git rev-parse main; git rev-parse origin/main
git rev-parse 'HEAD^{tree}'
git ls-remote origin refs/heads/main
git reflog --date=iso; git reflog show main --date=iso; git reflog show origin/main --date=iso
git cat-file -t a9289c2; git cat-file -e a9289c2
git branch -a --contains a9289c2
git merge-base a9289c2 4dcafb6
git fsck --unreachable --no-reflogs
git rev-parse 'e4c73ba^{tree}'; git rev-parse 'd8d4779^{tree}'   # 其余同理
git diff --stat a9289c2 4dcafb6; git diff --name-status a9289c2 4dcafb6
git diff --stat e4c73ba d8d4779; git diff --stat d3b3dca e5f33b6
git diff --stat e417073 56d1436; git diff --stat a9289c2 4dcafb6
git log --oneline 1b24195..4dcafb6
git diff --stat 1b24195 4dcafb6
```

### 8.2 构建/测试日志（build/stage-b，UTF-8 或 GBK 混合）

```text
r12-build.log                     r12-final-build.log            r12-acceptance-build.log
r12-verified-build.log            r12-at-most-once-build.log     r12-real-history-build.log
r12-real-history-final-build.log  r12-final-ctest.log            r12-ctest.log
r12-acceptance-ctest.log          r12-verified-ctest.log         r12-at-most-once-ctest.log
r12-final-memoryagent-ctest.log   r12-last-memoryagent-ctest.log
r12-acceptance-final/r12-matrix.log       r12-e417073/r12-matrix.log
r12-real-final/r12-matrix.log             r12-controlled/r12-matrix.log
r12-e417073/r12-contact-repeat-1.log      r12-e417073/r12-contact-repeat-2.log
r12-share-restart/r12-share-restart.log   r12-real/r12-real-model.log
r12-recovery-tests/{storage,outbox,storage-suite}.log
build/stage-b/tests/CTestTestfile.cmake   build/stage-b/.ninja_log
```

> 注意：`r12-matrix.log`、`r12-contact-repeat-*.log`、`r12-share-restart.log` 等含中文的日志为 **GBK/936** 编码；直接用 UTF-8 读取会报"invalid UTF-8"。核对时按 GBK 解码。`app_data/logs/logs_*.txt` 与 `r12-real-model.log` 为 **UTF-8**。

### 8.3 二进制与生产文件

```text
build/stage-b/Windows_AI.exe
build/stage-b/tests/memory-agent/tst_memoryagent.exe
build/stage-b/r12-*/tst_*.exe / Windows_AI.exe
build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/Windows_AI.exe
build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/tests/memory-agent/tst_memoryagent.exe
build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/app_data/config/{identity.json,setting.json}
build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug/app_data/memory/QianDaoMoZi_memory.db(+ .bak_*)
```

---

## 9. 核对局限与未能确认项（如实声明）

1. **二进制≠SHA**：exe 内无 commit SHA，§3.2 是内容级证据，不是形式化证明。
2. **构建日志不含 SHA**：日志只记 target/object/链接，不含提交号；树归属靠时间线 + 内容标记推断。
3. **无法确认 a9289c2 是否曾被其他机器推送**：本机 `origin/main` reflog 显示 `1b24195 → 4dcafb6` 直达；GitHub 端 reflog 不可得。
4. **本机重写具体工具/操作者无法确认**：reflog 只留操作名（"translate four local commit messages"、"match repository commit conventions"），无工具标识。
5. **生产环境 21:21 二进制的源码归属无法精确到 tree**：只能证明它早于 `e4c73ba`（缺少 `actionEvidenceBinding` 标记），不能定位到具体提交。
6. **工作区现场变化（并发 R12-3a，非本任务）**：核对开始时（23:08）工作区干净；随后出现并发改动：
   - 23:10:06 未跟踪 `src/cognition/cognitivecontext.h`（18,058 B）；
   - 23:12:10 未跟踪 `src/cognition/cognitivecontext.cpp`（105,470 B）；
   - 23:12:42 修改 `src/cognition/llmservice.h`；23:13:28 修改 `src/cognition/llmservice.cpp`（两者合计 `+308/−3`，均未提交）；
   - 23:13:32 修改 `CMakeLists.txt` 与 `tests/CMakeLists.txt`（未提交）；
   - 23:13:21 未跟踪 `docs/R12_3A_SOURCE_INVENTORY.md`（75,409 B）；
   - 以上为 23:16 快照；并发 R12-3a 改动仍在继续增加。
   - 本任务仅新增 `docs/R12_2C_FINAL_BASELINE.md`。
   
   因此：**此刻工作区不干净**；但 `HEAD`/`HEAD^{tree}` 未变（`4dcafb6`/`c239b5e7…`），本文件关于提交对象的结论不受影响。若日后要重建复验 R12-2c-final，应先取得 `4dcafb6` 的干净检出或等 R12-3a 落库，避免混入在途代码。
7. **未做任何重跑**：所有"通过"结论均来自**既有日志**，本轮未执行任何测试/模型调用。

---

## 10. 最终判定（一句话）

报告时刻的 `a9289c2` 与当前 `main=4dcafb6` **代码树完全一致（tree=c239b5e，整树 diff 为空）**；差异仅为两轮**本地提交信息重写**（且 a9289c2 从未被本机推送）。因此满足"只是 SHA 变化、代码树一致"的豁免条件，**无需重跑全部真实模型验收**；最终二进制与最终日志已覆盖最终树的真实模型矩阵（15/15）与 ctest（通过）。报告原文不可恢复，遗留风险 5 项按原文登记。

---

## 11. 后续更新（R12-3a 落地后补记）

- 本文件 §0-7/§9-6 记录的“并发 R12-3a 在途改动”已在 R12-3a 第一切片提交中落地，
  包括 `src/cognition/cognitivecontext.{h,cpp}`、`LLMService` shadow 接线、
  `tests/tst_cognitivecontext.cpp` 及本文件同期三份文档。该提交**未改动**本文件 §2 的
  Git 结论所依据的 `4dcafb6` 提交对象；但当前工作区 HEAD 会随 R12-3a 提交前移。
- 若日后要按本文件复验 R12-2c-final，仍应先从 `4dcafb6` 干净检出，或明确以
  “R12-3a 提交前的父提交 `4dcafb6`”为基线，不能直接用 R12-3a 之后的混合树复跑 23/23 口径。
