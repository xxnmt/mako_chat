#pragma once

#include "utterance.h"
#include "sentencedata.h"
#include "privatereview.h"
#include <QElapsedTimer>
#include <QObject>
#include <QQueue>
#include <optional>

class LLMService;
class MemoryManager;

class CognitiveCoordinator final : public QObject {
    Q_OBJECT
public:
    CognitiveCoordinator(MemoryManager &memory, LLMService &model, QObject *parent);
    ~CognitiveCoordinator() override;
    void submit(const QString &input, const RequestMeta &meta);
    void interrupt(const QString &reason);
    bool handedOff(const QString &input, const QString &reply, const QList<SentenceText> &sentences,
                   const RequestMeta &meta);
    bool busy() const { return active.has_value() || !pending.isEmpty(); }
    bool owns(const QString &run) const { return active && !finishing && active->meta.runId == run; }
    qint64 turnId() const { return active ? active->turnId : 0; }
    quint64 activeSequence() const { return active ? active->sequence : 0; }
    quint64 lastSequence() const { return sequence; }
    bool idleForWriting() const;
    void reviewIfDue();
signals:
    void contactProposed(qlonglong turn, int version);
    void draftReadyToShare(const QString &ref, int version, const QString &draft);
private:
    PrivateReview review;
    QElapsedTimer idle;
    struct Turn {
        QString text;
        RequestMeta meta;
        quint64 sequence;
        qint64 turnId;
    };
    void advance();
    void finish();
    bool finishing = false;
    MemoryManager &store;
    LLMService &service;
    QQueue<Turn> pending;
    std::optional<Turn> active;
    quint64 sequence = 0;
};
