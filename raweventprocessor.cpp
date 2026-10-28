#include "raweventprocessor.h"

#include "configmanager.h"
#include "llmservice.h"
#include "memorymanager.h"

#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

RawEventProcessor::RawEventProcessor(MemoryManager *memoryManager,
                                     LLMService *llmService,
                                     QObject *parent)
    : QObject(parent),
      m_memoryManager(memoryManager),
      m_llmService(llmService)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(30 * 1000);
    connect(m_timer, &QTimer::timeout, this, &RawEventProcessor::tick);
    if (m_llmService) {
        connect(m_llmService, &LLMService::interactionSummaryReady,
                this, &RawEventProcessor::onSummaryReady);
        connect(m_llmService, &LLMService::interactionSummaryFailed,
                this, &RawEventProcessor::onSummaryFailed);
    }
}

void RawEventProcessor::start()
{
    m_timer->start();
    QTimer::singleShot(3000, this, &RawEventProcessor::tick);
}

void RawEventProcessor::notifyNewEvent()
{
    // 定时器每 30 秒扫描；这里不立即分段，避免绕过 idle/batch 阈值。
}

void RawEventProcessor::processNow()
{
    tick();
}

void RawEventProcessor::tick()
{
    scan();
}

void RawEventProcessor::scan()
{
    if (!m_memoryManager || !m_llmService) {
        return;
    }

    const MemoryPolicyConfig policy =
        ConfigManager::instance().getMemoryPolicyConfig();
    if (!policy.longTermMemoryEnabled) {
        return;
    }

    const QQGroupMemoryConfig groups =
        ConfigManager::instance().getQQGroupMemoryConfig();
    if (!groups.enabled) {
        return;
    }

    const QList<QString> sessions = m_memoryManager->getPendingGroupSessions();
    for (const QString &sessionId : sessions) {
        if (!groups.groupAllow.contains(sessionId.toLongLong())) {
            continue;
        }
        const int count = m_memoryManager->countUnprocessedGroupEvents(sessionId);
        if (count <= 0) {
            continue;
        }
        const QDateTime latest =
            m_memoryManager->latestUnprocessedGroupEventTime(sessionId);
        const bool batchReady = count >= policy.interaction.groupBatchSize;
        const bool idleReady =
            latest.isValid()
            && latest.secsTo(QDateTime::currentDateTime())
                   >= policy.interaction.groupIdleMinutes * 60;
        if (batchReady || idleReady) {
            processSession(sessionId);
        }
    }

    processPendingSummaries();
}

void RawEventProcessor::processSession(const QString &sessionId)
{
    const MemoryPolicyConfig policy =
        ConfigManager::instance().getMemoryPolicyConfig();
    const QList<RawEvent> events = m_memoryManager->getUnprocessedGroupEvents(
        sessionId, policy.interaction.groupBatchSize);
    if (events.isEmpty()) {
        return;
    }

    const QList<InteractionDraft> drafts =
        InteractionSegmenter::segment(events, policy.interaction);
    for (const InteractionDraft &draft : drafts) {
        QList<qlonglong> eventIds;
        for (const QString &id : draft.sourceEventIds) {
            eventIds.append(id.toLongLong());
        }
        const qlonglong interactionId =
            m_memoryManager->createInteractionBatch(draft, eventIds);
        if (interactionId > 0) {
            qDebug() << "[RawEventProcessor] interaction 已创建:"
                     << interactionId << "session=" << sessionId
                     << "events=" << eventIds.size();
        }
    }
}

void RawEventProcessor::processPendingSummaries()
{
    if (m_summaryInFlight || !m_memoryManager || !m_llmService) {
        return;
    }
    const QList<qlonglong> ids = m_memoryManager->getPendingInteractionIds(1);
    if (ids.isEmpty()) {
        return;
    }

    const qlonglong id = ids.first();
    const QList<RawEvent> events =
        m_memoryManager->getInteractionSourceEvents(id);
    if (events.isEmpty()) {
        m_memoryManager->markInteractionSummaryFailed(id);
        return;
    }

    m_summaryInFlight = true;
    m_llmService->summarizeInteractionAsync(id, events);
}

void RawEventProcessor::onSummaryReady(qlonglong interactionId,
                                       const QString &summary,
                                       const QString &summaryFormat,
                                       const QString &segmentsJson,
                                       const QJsonArray &claims)
{
    const MemoryPolicyConfig policy =
        ConfigManager::instance().getMemoryPolicyConfig();
    const SanitizeResult sanitized = SensitiveContentFilter::sanitize(
        summary, QString(), policy.privacyRules);

    if (sanitized.blocked) {
        if (!m_memoryManager->saveInteractionSummaryWithClaims(
                interactionId, QString(), QStringLiteral("legacy"), QString(),
                {}, QStringLiteral("quarantine"))) {
            qDebug() << "[RawEventProcessor] interaction summary 隔离保存失败:"
                     << interactionId;
        }
        qDebug() << "[RawEventProcessor] interaction summary 隔离:"
                 << interactionId << "rules=" << sanitized.ruleIds;
    } else {
        QString safeSummary = sanitized.text;
        safeSummary.replace(QRegularExpression(QStringLiteral("@\\d{5,}")),
                            QStringLiteral("@某位成员"));
        safeSummary.replace(QRegularExpression(QStringLiteral("\\b\\d{5,}\\b")),
                            QStringLiteral("[成员]"));

        QJsonArray safeSegments;
        const QJsonArray rawSegments = QJsonDocument::fromJson(
            segmentsJson.toUtf8()).array();
        for (const QJsonValue &value : rawSegments) {
            QJsonObject segment = value.toObject();
            const SanitizeResult segmentSanitized = SensitiveContentFilter::sanitize(
                segment.value("text").toString(), QString(), policy.privacyRules);
            if (segmentSanitized.blocked) {
                continue;
            }
            QString text = segmentSanitized.text;
            text.replace(QRegularExpression(QStringLiteral("@\\d{5,}")),
                         QStringLiteral("@某位成员"));
            text.replace(QRegularExpression(QStringLiteral("\\b\\d{5,}\\b")),
                         QStringLiteral("[成员]"));
            if (text.trimmed().isEmpty()) {
                continue;
            }
            segment["text"] = text;
            safeSegments.append(segment);
        }

        QList<ClaimDraft> safeClaims;
        for (const QJsonValue &value : claims) {
            const QJsonObject obj = value.toObject();
            const SanitizeResult claimSanitized = SensitiveContentFilter::sanitize(
                obj.value("statement").toString(), QString(), policy.privacyRules);
            if (claimSanitized.blocked || claimSanitized.text.trimmed().isEmpty()) {
                continue;
            }
            ClaimDraft claim;
            claim.assertedByPersonId =
                obj.value("asserted_by_person_id").toString();
            claim.aboutPersonId = obj.value("about_person_id").toString();
            claim.privacyOwnerPersonId =
                obj.value("privacy_owner_person_id").toString();
            claim.counterpartPersonId =
                obj.value("counterpart_person_id").toString();
            claim.statement = claimSanitized.text;
            claim.privacyLevel = qBound(1, obj.value("privacy_level").toInt(1), 5);
            claim.claimStatus = obj.value("claim_status").toString(
                QStringLiteral("quarantine"));
            safeClaims.append(claim);
        }

        const bool hasSegments = !safeSegments.isEmpty();
        const QString safeSegmentsJson = hasSegments
                                             ? QString::fromUtf8(
                                                   QJsonDocument(safeSegments)
                                                       .toJson(QJsonDocument::Compact))
                                             : QString();
        if (!m_memoryManager->saveInteractionSummaryWithClaims(
                interactionId, safeSummary,
                hasSegments ? QStringLiteral("person_refs") : summaryFormat,
                safeSegmentsJson, safeClaims, QStringLiteral("ready"))) {
            qDebug() << "[RawEventProcessor] interaction summary 保存失败:"
                     << interactionId;
            m_memoryManager->markInteractionSummaryFailed(interactionId);
        } else {
            qDebug() << "[RawEventProcessor] interaction summary 已保存:"
                     << interactionId << "chars=" << safeSummary.size()
                     << "segments=" << safeSegments.size()
                     << "claims=" << safeClaims.size();
        }
    }

    m_summaryInFlight = false;
    processPendingSummaries();
}

void RawEventProcessor::onSummaryFailed(qlonglong interactionId,
                                        const QString &reason)
{
    m_memoryManager->markInteractionSummaryFailed(interactionId);
    qDebug() << "[RawEventProcessor] interaction summary 失败:"
             << interactionId << reason;
    m_summaryInFlight = false;
    QTimer::singleShot(5000, this, &RawEventProcessor::processPendingSummaries);
}
