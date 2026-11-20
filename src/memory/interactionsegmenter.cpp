#include "interactionsegmenter.h"

#include "qqmessageparser.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

QString replyTargetFromPayload(const QString &payloadJson)
{
    if (payloadJson.isEmpty()) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(payloadJson.toUtf8());
    if (!doc.isObject()) {
        return {};
    }
    return QQMessageParser::parse(doc.object()).replyToMessageId;
}

} // namespace

QList<InteractionDraft> InteractionSegmenter::segment(
    const QList<RawEvent> &events,
    const InteractionConfig &config)
{
    QList<InteractionDraft> drafts;
    if (events.isEmpty()) {
        return drafts;
    }

    InteractionDraft current;
    QSet<QString> participantIds;
    QSet<QString> messageIds;
    QDateTime lastTime;
    bool hasExplicitLink = false;

    auto flush = [&]() {
        if (current.sourceEventIds.isEmpty()) {
            return;
        }
        current.segmentationMethod = hasExplicitLink
                                         ? QStringLiteral("explicit_link")
                                         : QStringLiteral("time_window");
        current.confidence = hasExplicitLink ? 0.8 : 0.5;
        drafts.append(current);
        current = InteractionDraft{};
        participantIds.clear();
        messageIds.clear();
        lastTime = QDateTime();
        hasExplicitLink = false;
    };

    for (const RawEvent &event : events) {
        if (event.eventType != QStringLiteral("message")) {
            continue;
        }

        const QString replyTarget = replyTargetFromPayload(event.payloadJson);
        const bool explicitLink = !replyTarget.isEmpty()
                                  && messageIds.contains(replyTarget);

        bool shouldFlush = false;
        if (!current.sourceEventIds.isEmpty()) {
            if (current.sourceEventIds.size() >= config.maxEvents) {
                shouldFlush = true;
            } else if (current.startedAt.isValid()
                       && current.startedAt.secsTo(event.occurredAt)
                              > config.maxMinutes * 60) {
                shouldFlush = true;
            } else if (!explicitLink && lastTime.isValid()
                       && lastTime.secsTo(event.occurredAt) > config.gapSeconds) {
                shouldFlush = true;
            }
        }
        if (shouldFlush) {
            flush();
        }

        if (current.sourceEventIds.isEmpty()) {
            current.channel = event.channel;
            current.sessionId = event.sessionId;
            current.startedAt = event.occurredAt.isValid()
                                    ? event.occurredAt
                                    : QDateTime::currentDateTime();
        }

        current.endedAt = event.occurredAt.isValid()
                              ? event.occurredAt
                              : QDateTime::currentDateTime();
        current.sourceEventIds.append(QString::number(event.id));
        if (!event.messageId.isEmpty()) {
            messageIds.insert(event.messageId);
        }
        if (explicitLink) {
            hasExplicitLink = true;
        }
        if (!event.senderId.isEmpty() && !participantIds.contains(event.senderId)) {
            participantIds.insert(event.senderId);
            current.participants.append(
                qMakePair(event.senderId, event.senderName));
        }
        lastTime = current.endedAt;
    }
    flush();
    return drafts;
}
