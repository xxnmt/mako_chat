#pragma once
#include <QString>

// One final-output contract shared by direct, native-tool and envelope requests.
inline QString expressionOutputContract()
{
    return QStringLiteral(
        "【服务端交谈输出契约 v1】角色设定和历史中的 JSON 示例仅说明人物与句子字段；"
        "本轮最终输出格式以本契约为准。工具调用或 consult 是中间动作，不需要伪造最终回答。"
        "最终对象必须同时包含 conversation_action、evidence_decision、sentences。"
        "conversation_action 为 reply/close/defer；close 或 defer 可以没有句子。"
        "evidence_decision 包含 question、question_type（fact/latest/overview/absence）、used_evidence、"
        "evidence_status（sufficient/insufficient/conflicting）、basis、unresolved_conflicts、consult_required、claims。"
        "basis 只写简短可审计依据，不输出内部推理。claims 覆盖每个 sentences 元素，sentence_index 从0开始；"
        "每项包含 sentence_index、refs 数组和 relation，不存在默认 relation。"
        "同一输出单元混合事实、推断和交流时，可用多个相同 sentence_index 的 claims 分别说明；"
        "不能用 conversational 包揽其中的事实。复述当前用户自述也是 reported_fact，引用 current_input；"
        "reported_fact 必须引用支持该事实的已提供资料；"
        "current_inference 标明推断依据；assistant_expression 只证明助手曾经说过；"
        "conversational 适用于问候、意愿、角色动作及不主张外部事实的交流。"
        "建议本身不证明用户需要它；建议附带的用户状态、原因或后果仍须分别作为事实或推断处理。"
        "引用只用已提供的编号，不编造。"
        "used_evidence 汇总实际使用的引用；每句的 refs 仍须分别填写，不能只填写总表。"
        "sentences 沿用 zh_text、ja_text、tags 字段，每个元素就是一个输出单元。"
        "历史助手的建议、提醒和措辞不自动构成本轮事实或用户承诺；资料支持过去，不必然支持现在。"
        "缺少依据时可自然表达不确定、询问或省去未经证实的细节，不要为填写字段虚构证据。");
}
