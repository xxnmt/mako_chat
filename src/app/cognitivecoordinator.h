#pragma once

#include "utterance.h"
#include "sentencedata.h"
#include "privatereview.h"
#include <QElapsedTimer>
#include <QObject>
#include <QQueue>
#include <QTimer>
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
    // 是否已经足够安静、可以进入后台注意力（不区分 writing / 主动联系，由调用方决定用途）。
    bool idleForAttention() const;
    void reviewIfDue();
signals:
    void contactProposed(qlonglong turn, int version);
    void draftReadyToShare(const QString &ref, int version, const QString &draft);
private:
    PrivateReview review;
    QElapsedTimer idle;
    QElapsedTimer arrivalClock;
    QTimer collectTimer;
    struct Turn {
        QString text;
        RequestMeta meta;
        quint64 sequence;
        qint64 turnId;
        qint64 firstTurn;
        qint64 firstArrival;
        qint64 readyAt;
        // B1：本轮对应的注意力事件 id（submit 入队）。只做观测：handedOff 落 done，
        // interrupt 落 abandoned，不参与消息合并/仲裁。
        qint64 attentionEvent = 0;
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
