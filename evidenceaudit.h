#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <cmath>

// Structural accounting only. Resolving a reference never proves entailment.
inline QJsonObject auditExpressionClaims(const QJsonObject &decision, const QJsonObject &evidence)
{
    const auto report = decision["evidence_decision"].toObject();
    const auto sentences = decision["sentences"].toArray();
    const auto claims = report["claims"].toArray();
    QJsonArray issues, missing;
    QSet<int> covered;
    if (report.contains("claims") && !report["claims"].isArray()) issues.append("invalid_claims");
    for (const auto &item : claims) {
        const auto claim = item.toObject();
        const auto value = claim["sentence_index"];
        if (!item.isObject() || !value.isDouble() || value.toDouble()!=std::floor(value.toDouble())
            || value.toDouble()<0 || value.toDouble()>=sentences.size()) {
            issues.append("unknown_sentence");
            continue;
        }
        covered.insert(value.toInt());
        const auto relation = claim["relation"].toString();
        if (!QSet<QString>{"reported_fact","current_inference","assistant_expression","conversational"}.contains(relation))
            issues.append("invalid_relation");
        const auto refs = claim["refs"].toArray();
        if (!claim["refs"].isArray()) issues.append("invalid_references");
        if (relation!="conversational" && refs.isEmpty()) issues.append("missing_support");
        for (const auto &ref : refs) {
            const auto record = evidence[ref.toString()].toObject();
            if (!ref.isString() || record.isEmpty()) issues.append("unknown_claim_reference");
            else if (relation=="reported_fact" && (record["speaker_role"]=="assistant"
                || record["scope"]=="prior_assistant_expression" || record["certainty"]=="inferred"
                || record["source_kind"]=="inference")) issues.append("scope_does_not_support_fact");
        }
    }
    for (int i=0; i<sentences.size(); ++i) if (!covered.contains(i)) missing.append(i);
    const QString check = !issues.isEmpty() ? "unsupported" : claims.isEmpty() ? "not_reported"
        : !missing.isEmpty() ? "incomplete" : "structural_checks_passed";
    return {{"claim_check",check},{"claim_issues",issues},{"unaccounted_sentences",missing},
            {"sentence_count",sentences.size()},{"accounted_sentence_count",covered.size()},
            {"semantic_validation","not_performed"}};
}
