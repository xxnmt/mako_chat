#ifndef TTSPROCESSMANAGER_H
#define TTSPROCESSMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>

class TTSProcessManager : public QObject
{
    Q_OBJECT
public:
    explicit TTSProcessManager(const QString &apiPythonPath,const QString &scriptDir,
                               const QStringList &args,int port=9880,QObject *parent = nullptr);
    ~TTSProcessManager();

    bool isApiReady();
    void apiStart();
    void apiStop();
signals:
    void apiReady();
    void apiFailed(const QString &error);
    void statusMessage(const QString &msg);
private slots:
    void onReadyReadStandOutput();
    void onProcessStateChanged(QProcess::ProcessState state);
    void onHealthCheck();
private:
    QProcess *m_process=nullptr;
    QTimer *m_timer=nullptr;
    QTimer m_startDeadline;
    quint64 m_generation = 0;
    bool m_isApiReady=false;
    QString m_apiPythonPath;
    QString m_scriptDir;
    QStringList m_args;
    int m_port;
    const QString READY_MARKER = "Uvicorn running on";
    void fail(const QString &reason);

};

#endif // TTSPROCESSMANAGER_H
