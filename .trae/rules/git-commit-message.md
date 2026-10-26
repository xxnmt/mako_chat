---
alwaysApply: true
scene: git_message
---

在此处编写规则，自定义 AI 生成提交信息的风格。
仅中文提交信息格式：

## 提交信息结构

```
<type>(<scope>): <中文描述> | <English description>

<可选正文 Chinese body>

<可选正文 English body>

<可选页脚 footer>
```

## 类型定义 (Type)

| Type | 中文说明 | English Description |
|------|----------|---------------------|
| `feat` | 新功能 | A new feature |
| `fix` | 修复Bug | A bug fix |
| `docs` | 文档变更 | Documentation only changes |
| `style` | 代码格式 | Changes that do not affect the meaning of the code |
| `refactor` | 重构 | A code change that neither fixes a bug nor adds a feature |
| `perf` | 性能优化 | A code change that improves performance |
| `test` | 测试相关 | Adding missing tests or correcting existing tests |
| `build` | 构建系统 | Changes that affect the build system or external dependencies |
| `ci` | CI配置 | Changes to CI configuration files and scripts |
| `chore` | 杂项 | Other changes that don't modify src or test files |
| `revert` | 回滚 | Reverts a previous commit |

## 作用域 (Scope)

作用域用于指定提交影响的模块，可选项：
- `widget` - 主窗口组件
- `bubble` - 气泡组件
- `character` - 角色组件
- `image` - 图片资源
- `cmake` - CMake构建配置
- `git` - Git相关配置

## 格式规范

1. **标题行**: 不超过 72 个字符，中英文用 ` | ` 分隔
2. **正文**: 每行不超过 100 个字符，解释"为什么"而不是"做了什么"
3. **页脚**: 用于标注 Breaking Change 或关联 Issue
4. **时态**: 使用现在时，而非过去时
5. **大小写**: 中文正常书写，英文首字母大写

## 示例

### 新功能
```
feat(widget): 添加窗口透明度控制 | Add window opacity control

支持通过滑块调整主窗口透明度，范围10%-100%
Support adjusting main window opacity via slider, range 10%-100%
```

### Bug修复
```
fix(bubble): 修复气泡闪烁问题 | Fix bubble flickering issue

原因是定时器间隔过短导致重绘冲突
The cause was timer interval too short causing repaint conflicts
```

### 文档更新
```
docs: 更新README使用说明 | Update README usage instructions
```

### 构建配置
```
build(cmake): 升级Qt版本要求至6.5 | Upgrade Qt version requirement to 6.5
```

### 杂项
```
chore(git): 添加.gitignore并清理构建产物 | Add .gitignore and clean up build artifacts
```

## 禁止事项

- 不要使用 `update`、`modify` 等模糊词汇作为 type
- 不要在标题行末尾加句号
- 不要提交包含敏感信息的提交信息
- 不要使用 emojis（除非用户明确要求）
