#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include "memorymanager.h"
#include "recentcontext.h"
#include "utterance.h"

// 与 tst_memorymanager 一致的原始连接辅助：独立于 MemoryManager 直接读写测试库，
// 用于校验表内容、把回顾冷却时间往前挪。
// 注意：QSqlDatabase 句柄必须在 removeDatabase 前析构（close() 内先释放 m_db）。
class RawDb
{
public:
    RawDb(const QString &path, const QString &tag)
        : m_conn(QStringLiteral("tst_concern_raw_") + tag)
    {
        m_db = QSqlDatabase::addDatabase("QSQLITE", m_conn);
        m_db.setDatabaseName(path);
    }
    ~RawDb() { close(); }

    bool open() { return m_db.open(); }
    bool exec(const QString &sql)
    {
        QSqlQuery q(m_db);
        return q.exec(sql);
    }
    QVariant scalar(const QString &sql)
    {
        QSqlQuery q(m_db);
        if (!q.exec(sql) || !q.next()) {
            qWarning() << "scalar query failed:" << sql << q.lastError().text();
            return {};
        }
        return q.value(0);
    }
    QString lastError() const { return m_db.lastError().text(); }
    void close()
    {
        if (m_db.isOpen()) {
            m_db.close();
        }
        m_db = QSqlDatabase();
        if (QSqlDatabase::contains(m_conn)) {
            QSqlDatabase::removeDatabase(m_conn);
        }
    }

private:
    QString m_conn;
    QSqlDatabase m_db;
};

namespace {

// 提示"荷包蛋"这件事的原始发言（关注来源轮次 A 的内容）。
const QString kConcernInput = QStringLiteral("明天想做荷包蛋，不知道好不好吃");
// 关注本身：note 里同时含"荷包蛋"和"好不好吃"，供后续发言片段打分。
const QString kConcernNote = QStringLiteral("荷包蛋好不好吃");
// 后来的一句普通回答（轮次 B 的内容），不提供任何 ranked 命中轮次。
const QString kAnswerInput = QStringLiteral("很好吃哦，香香的油油的，不过没有荷包蛋呀");

// 新建一条"用户刚说出口"的轮次元数据。回顾来源必须落在最近 24 小时内，
// 因此 receivedAtUtc 固定为当前时间（enqueueObservation 用它填 cognitive_source.reported）。
RequestMeta turnMeta(const QString &event)
{
    RequestMeta meta;
    meta.eventId = meta.runId = event;
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    return meta;
}

// 写入一条用户消息并收口为 quiet：只有 handed_off/quiet 的轮次才能被回顾领取。
// 返回 0 表示写入失败，由调用方 QVERIFY 报告。
qint64 recordQuietTurn(MemoryManager &mm, const RequestMeta &meta, const QString &text)
{
    const qint64 turn = mm.recordInput(text, meta, true);
    if (turn <= 0) return 0;
    return mm.recordQuiet(turn, "close") ? turn : 0;
}

// 领取并提交一条 consider 回顾，形成一个开放关注；返回关注来源轮次，0 表示失败。
qint64 seedOpenConcern(MemoryManager &mm, const RequestMeta &meta,
                       const QString &input, const QString &note)
{
    const qint64 turn = recordQuietTurn(mm, meta, input);
    if (turn <= 0) return 0;
    const auto claim = mm.claimPrivateReflection(4, 60);
    if (claim["id"].toInteger() != turn || claim["version"].toInt() != 1) return 0;
    if (!mm.finishPrivateReflection(turn, 1, "consider", note)) return 0;
    return turn;
}

// 领取回顾有 60 秒冷却：把 thought_review_log 的最后开始时间往前挪 120 秒，模拟冷却已过。
bool rewindReviewCooldown(const QString &dbPath, const QString &tag)
{
    RawDb clock(dbPath, tag);
    if (!clock.open()) return false;
    return clock.exec(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120"));
}

} // namespace

class TestConcern : public QObject
{
    Q_OBJECT

private slots:
    void concernMatchedWithoutRetrievalHit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-match.db");
        MemoryManager db(path);

        // 关注来源：轮次 A 被回顾成一条开放的"荷包蛋好不好吃"关注。
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-match-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);

        // 轮次 B 是后来的一句普通回答，不给检索任何命中轮次。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-match-clock")));
        const qint64 bTurn = recordQuietTurn(db, turnMeta(QStringLiteral("concern-match-b")),
                                             kAnswerInput);
        QVERIFY(bTurn > 0);
        const auto claimB = db.claimPrivateReflection(4, 60);
        QCOMPARE(claimB["id"].toInteger(), bTurn);
        QCOMPARE(claimB["version"].toInt(), 1);

        // 核心回归：rankedTurns 为空时，旧实现会因"检索未命中"错过 prior；
        // 新实现应凭关注 note 与当前发言的相关性给出 match。
        const auto history = db.reflectionHistory(bTurn, 1, {});
        QVERIFY(history.contains("prior"));
        const auto prior = history["prior"].toObject();
        QCOMPARE(prior["source_turn"].toInteger(), aTurn);
        QCOMPARE(prior["own_reflection"].toString(), kConcernNote);
        QCOMPARE(prior["version"].toInteger(), qlonglong(1));
        QCOMPARE(history["prior_reason"].toString(), QStringLiteral("match"));
        QCOMPARE(history["concern_matches"].toInt(), 1);
    }

    void matchedConcernsScoresByQueryText()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("concern-score.db"));
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-score-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);

        // matchedConcerns 要求 terms 非空（空列表直接返回空），这里用与前台一致的抽词；
        // 真正的命中来自 note 片段对整句 queryText 的打分。
        const QStringList terms = RecentContextPlanner::extractTerms(kAnswerInput.left(100));
        QVERIFY(!terms.isEmpty());
        RequestMeta viewer;
        const auto matched = db.matchedConcerns(viewer, terms, kAnswerInput, 0, 2);
        QVERIFY(!matched.isEmpty());
        const auto concern = matched.first().toObject();
        QCOMPARE(concern["source_turn"].toInteger(), aTurn);
        QVERIFY(concern["match_score"].toDouble() > 0.0);
        QVERIFY(concern["matched_terms"].isArray());

        // 同一份开放关注，换成一段与 note 完全无关的输入（含其抽词）后不应再命中。
        // 注意：抽词与 queryText 都必须来自无关文本，否则旧候选仍可能靠词面重合命中。
        const QString unrelatedText = QStringLiteral("今天天气不错，适合出门散步");
        const QStringList unrelatedTerms = RecentContextPlanner::extractTerms(unrelatedText.left(100));
        QVERIFY(!unrelatedTerms.isEmpty());
        QVERIFY(db.matchedConcerns(viewer, unrelatedTerms, unrelatedText, 0, 2).isEmpty());
    }

    void resolveConcernFromConversation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-resolve.db");
        MemoryManager db(path);
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-resolve-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);
        const qint64 bTurn = recordQuietTurn(db, turnMeta(QStringLiteral("concern-resolve-b")),
                                             kAnswerInput);
        QVERIFY(bTurn > 0);

        QVERIFY(db.resolveConcern(aTurn, bTurn, "answer", "current_input"));

        // private_reflection 里关注状态应为 resolved，concern_resolution 记录"由轮次 B 了结轮次 A"。
        RawDb raw(path, QStringLiteral("concern-resolve-raw"));
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(aTurn)).toString(),
                 QStringLiteral("resolved"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM concern_resolution "
                                           "WHERE turn=%1 AND prior_turn=%2 AND action='resolve' AND kind='answer'")
                                .arg(bTurn).arg(aTurn)).toInt(),
                 1);

        // 已了结的关注不再出现在前台注入与近期回顾里。
        RequestMeta viewer;
        const QStringList terms = RecentContextPlanner::extractTerms(kAnswerInput.left(100));
        QVERIFY(!terms.isEmpty());
        QVERIFY(db.matchedConcerns(viewer, terms, kAnswerInput, 0, 2).isEmpty());
        QVERIFY(db.recentPrivateReflection(viewer).isEmpty());
    }

    void resolveRequiresEvidence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("concern-evidence.db"));
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-evidence-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);
        const qint64 bTurn = recordQuietTurn(db, turnMeta(QStringLiteral("concern-evidence-b")),
                                             kAnswerInput);
        QVERIFY(bTurn > 0);

        // 空证据：了结关注必须指明证据来源，不能凭空的"答案"。
        QVERIFY(!db.resolveConcern(aTurn, bTurn, "answer", ""));
        // 非法 kind：只接受 answer/contradiction/cancelled/dropped。
        QVERIFY(!db.resolveConcern(aTurn, bTurn, "bogus", "current_input"));

        // 两次非法调用都不能改变关注状态，也不能留下了结记录。
        RawDb raw(dir.filePath("concern-evidence.db"), QStringLiteral("concern-evidence-raw"));
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(aTurn)).toString(),
                 QStringLiteral("consider"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM concern_resolution WHERE prior_turn=%1")
                                .arg(aTurn)).toInt(),
                 0);
        QVERIFY(!db.concernByHead(aTurn).isEmpty());
    }

    void doubleResolveRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-double.db");
        MemoryManager db(path);
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-double-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);
        const qint64 bTurn = recordQuietTurn(db, turnMeta(QStringLiteral("concern-double-b")),
                                             kAnswerInput);
        QVERIFY(bTurn > 0);

        QVERIFY(db.resolveConcern(aTurn, bTurn, "answer", "current_input"));
        // 第二次了结同一个关注必须被拒绝，且不产生第二条 concern_resolution。
        QVERIFY(!db.resolveConcern(aTurn, bTurn, "answer", "current_input"));

        RawDb raw(path, QStringLiteral("concern-double-raw"));
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(aTurn)).toString(),
                 QStringLiteral("resolved"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM concern_resolution WHERE prior_turn=%1")
                                .arg(aTurn)).toInt(),
                 1);
    }

    void finishPrivateReflectionMatchesPrior()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-prior.db");
        MemoryManager db(path);
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-prior-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);

        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-prior-clock")));
        const QString cText = QStringLiteral("荷包蛋真的很好吃，下次还买");
        const qint64 cTurn = recordQuietTurn(db, turnMeta(QStringLiteral("concern-prior-c")), cText);
        QVERIFY(cTurn > 0);
        const auto claimC = db.claimPrivateReflection(4, 60);
        QCOMPARE(claimC["id"].toInteger(), cTurn);
        QCOMPARE(claimC["version"].toInt(), 1);

        const QStringList terms = RecentContextPlanner::extractTerms(cText.left(100));
        QVERIFY(!terms.isEmpty());
        const auto history = db.reflectionHistory(cTurn, 1, {}, terms);
        QVERIFY(history.contains("prior"));
        const auto prior = history["prior"].toObject();
        QCOMPARE(prior["source_turn"].toInteger(), aTurn);
        QCOMPARE(history["prior_reason"].toString(), QStringLiteral("match"));

        // 带着 prior 提交本轮回顾：选择 rest，同时把旧关注标记为 resolve，应写入 reflection_change。
        QVERIFY(db.finishPrivateReflection(cTurn, 1, "rest", "", {},
                                           history["history"].toArray(), prior, "resolve"));

        RawDb raw(path, QStringLiteral("concern-prior-raw"));
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_change "
                                           "WHERE turn=%1 AND prior_turn=%2 AND action='resolve'")
                                .arg(cTurn).arg(aTurn)).toInt(),
                 1);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(cTurn)).toString(),
                 QStringLiteral("rest"));
    }

    void priorReasonNoneWhenUnrelated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-none.db");
        MemoryManager db(path);
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-none-a")),
                                             kConcernInput, kConcernNote);
        QVERIFY(aTurn > 0);

        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-none-clock")));
        const qint64 eTurn = recordQuietTurn(db, turnMeta(QStringLiteral("concern-none-e")),
                                             QStringLiteral("今天天气不错，适合出门散步"));
        QVERIFY(eTurn > 0);
        const auto claimE = db.claimPrivateReflection(4, 60);
        QCOMPARE(claimE["id"].toInteger(), eTurn);
        QCOMPARE(claimE["version"].toInt(), 1);

        // 新消息与旧关注无关：没有 prior，也没有可用的 history 行；
        // prior_reason 只能落在 none/ranked 之一（当前实现在 rankedTurns 为空时为 none）。
        const auto history = db.reflectionHistory(eTurn, 1, {});
        QVERIFY(!history.contains("prior"));
        const QString reason = history["prior_reason"].toString();
        QVERIFY(reason == QStringLiteral("none") || reason == QStringLiteral("ranked"));
        QVERIFY(history["history"].toArray().isEmpty());

        // 旧关注仍保持开放（没有 reflection_change），可被前台看到。
        const auto stillOpen = db.concernByHead(aTurn);
        QVERIFY(!stillOpen.isEmpty());
        QCOMPARE(stillOpen["source_turn"].toInteger(), aTurn);
        QCOMPARE(stillOpen["own_reflection"].toString(), kConcernNote);
    }

    void commonWordsDoNotMatch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("concern-common.db"));
        const qint64 aTurn = seedOpenConcern(db, turnMeta(QStringLiteral("concern-common-a")),
                                             QStringLiteral("今天吃了吗"),
                                             QStringLiteral("今天吃了吗"));
        QVERIFY(aTurn > 0);
        RequestMeta viewer;
        // "今天"这类常见词单独出现，不足以把一句无关寒暄匹配成同一条关注。
        QVERIFY(db.matchedConcerns(viewer, {}, QStringLiteral("今天天气不错，适合出门散步")).isEmpty());
        // 反过来，真正含同一件事的句子仍然命中（防止把匹配器收紧到失效）。
        QVERIFY(!db.matchedConcerns(viewer, {}, QStringLiteral("今天吃了吗，吃的什么")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestConcern)
#include "tst_concern.moc"
