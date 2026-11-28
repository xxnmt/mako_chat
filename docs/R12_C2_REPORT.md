# R12-C2 主模型记忆补查

评审基线为 64715b876da57a73a12442ad250e9b945fd2e3e6。

## 退役边界

|基线入口与职责|本轮处理|
|---|---|
|LLMService::prepareReply 的 legacy 分支，MemoryAgentPlanner::shouldRun / isMemoryQuestion|删除独立规划调度和回忆关键词门控。MemoryAgentTools::canExposeTools 只检查开关、渠道、轮次类型，调用方检查长期记忆权限。|
|MemoryAgent::start / sendPlanner / handlePlannerReply，V1 工具循环和 V2 计划响应|删除专属网络客户端、规划提示词、响应处理、超时和审计对象，以及 LLMService 接收规划结果再恢复请求的回调。|
|MemoryAgentTools 内部检索、MemoryRecallPlan、MemoryExecutor、时间解析|保留。主模型通过 MemoryRecallTool 查询或读取证据，服务端构造身份、授权范围、时间和预算。|
|LLMService 自动证据准备、当轮认识、群近期上下文、后台观察|保留原有入口。前台在工具选择前仍可见当前资料，允许直接答复。|
|memory.agent_mode / agent_plan_v2 / agent_trigger|设置页和默认序列化退役。加载 legacy 时记录兼容诊断，按用户的 agent_enabled、agent_protocol、长期记忆和渠道权限启用工具。保存时原始旧键由已有合并逻辑保留。|
|memory.agent_timeout_ms|保留为定向检索执行预算，概览仍使用自身查询。|

## 协议与回归

前台原生请求使用 tool_choice=auto。主模型返回调用后，LLMService::handleNativeToolCalls 执行检索，并以同一个调用 ID 返回工具结果。JSON 信封由 handleEnvelopeRecall 解析，共享 needs 执行器；每次返回结果后均可继续补查，受轮数、共享查询额度和重复查询限制。版本冲突、回顾退避和主动联系执行路径沿用 C1。

foregroundRecallContinuations 使用隔离数据库、本地 HTTP 服务和原始 legacy 设置。对于“那次碰面约在哪里，下雨又去哪里”，模拟模型先后选择“集合点”和“备用地点”。两个协议都实际执行两次检索。测试逐条检查后继请求中的工具结果数量、历史正文及最终表达。该试验验证模型作出选择后的程序链路。另有跨渠道更正、不同听众、无工具合法回复、OpenWake 到前台的连续性、活动成果及回执用例。

realModelActualToolRecall 的验收依据改为带真实调用 ID 的 memory_recall 工具响应。仅在提示里找到工具名不能证明发生补查。隔离测例只使用合成的出游记录。隔离进程缺少验收环境变量，该用例跳过。用户随后授权只读复用生产凭据。执行环境的自动审批再次以 blocked by policy 拒绝了读取并向子进程注入凭据的命令，实际模型请求没有发出。真实 provider 的选择仍待验证。

用户提供的约 10 小时 27 分钟日志属于改动前基线。14 轮均暴露工具、零次实际调用，属于自然使用观测。主动联系核验、上下文重复、重复提醒和活动主题单一继续保留为使用观察项。

最终隔离构建 build/stage-b/Windows_AI.exe 的 SHA-256 为 F4C9DF6B2640DB8CE88818D30AAED7C0FD6CFDD15FB3B5385816CEAC1B70AB19。全量 CTest 24/24 通过，日志位于 build/stage-b/c2-complete-ctest.log。生产配置两份文件的 SHA-256 与 C1 校验值一致。
