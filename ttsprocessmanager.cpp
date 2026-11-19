#include "ttsprocessmanager.h"
#include <QDebug>
#include <QTcpSocket>



TTSProcessManager::TTSProcessManager(const QString &apiPythonPath, const QString &scriptDir,
                                     const QStringList &args,int port, QObject *parent):
    QObject(parent),
    m_apiPythonPath(apiPythonPath),
    m_scriptDir(scriptDir),
    m_args(args),
    m_port(port)
{
    m_startDeadline.setSingleShot(true);
    connect(&m_startDeadline, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("TTS startup timed out"));
    });
    qDebug()<<"[TTSPM]:以构建,apiPath:"<<m_apiPythonPath;
}

TTSProcessManager::~TTSProcessManager()
{
    apiStop();
    qDebug()<<"[TTSPM]:api已关闭";
}

bool TTSProcessManager::isApiReady()
{
    return m_isApiReady;
}

void TTSProcessManager::apiStart()
{
    if (m_isApiReady || m_startDeadline.isActive()) return;
    QTcpSocket probe;
    probe.connectToHost("127.0.0.1", m_port);
    if (probe.waitForConnected(500)) {
        emit apiFailed(QStringLiteral("TTS port already occupied by an external service"));
        return;
    }

    if(m_process){
        if(m_process->state()==QProcess::Running){
            qDebug()<<"[TTSPM]:服务已启动，无需重复启动";
            return;
        }
        delete m_process;
        m_process =nullptr;
    }
    m_process=new QProcess(this);
    m_process->setWorkingDirectory(m_scriptDir);
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process,&QProcess::readyReadStandardOutput,this,&TTSProcessManager::onReadyReadStandOutput);
    connect(m_process,&QProcess::stateChanged,this,&TTSProcessManager::onProcessStateChanged);

    QStringList fullArgs=m_args;
    fullArgs.prepend(m_scriptDir+"/api_v2.py");
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("TTS process failed to start"));
    });
    ++m_generation;
    m_startDeadline.start(60000);
    m_process->start(m_apiPythonPath,fullArgs);
    if(!m_timer){
        m_timer=new QTimer(this);
        m_timer->setInterval(1000);
        connect(m_timer,&QTimer::timeout,this,&TTSProcessManager::onHealthCheck);
    }
    m_timer->start();

    qDebug()<<"[TTSPM]:api启动中，请稍后";
}

void TTSProcessManager::apiStop()
{
    ++m_generation;
    m_startDeadline.stop();
    if(m_timer){
        m_timer->stop();
        delete m_timer;
        m_timer=nullptr;
    }
    if(m_process){
        disconnect(m_process, nullptr, this, nullptr);
        m_process->terminate();
        if(!m_process->waitForFinished(3000)){
            m_process->kill();
            m_process->waitForFinished(3000);
        }
        m_process->deleteLater();
        m_process=nullptr;
    }
    m_isApiReady=false;
}

void TTSProcessManager::onReadyReadStandOutput()
{
    QString data=QString::fromUtf8(m_process->readAllStandardOutput());
    emit statusMessage(data);
    if(!m_isApiReady&&data.contains(READY_MARKER)){
        qDebug()<<"[TTSPM](stdOut):api启动成功";
        m_isApiReady=true;
        m_startDeadline.stop();
        if(m_timer) m_timer->stop();

        emit apiReady();
    }
}

void TTSProcessManager::onProcessStateChanged(QProcess::ProcessState state)
{
    if (state == QProcess::NotRunning && (m_isApiReady || m_startDeadline.isActive()))
        fail(QStringLiteral("TTS process exited"));
}

void TTSProcessManager::onHealthCheck()
{
    if(m_isApiReady) {
        m_timer->stop();
        return;
    }
    // 异步探测：不阻塞主线程，避免拖动人物卡顿
    QTcpSocket *socket = new QTcpSocket(this);
    const quint64 generation = m_generation;
    connect(socket, &QTcpSocket::connected, this, [this, socket, generation]() {
        if (generation == m_generation && !m_isApiReady && m_startDeadline.isActive()) {
            m_isApiReady = true;
            m_startDeadline.stop();
            if (m_timer) m_timer->stop();
            qDebug()<<"[TTSPM]（tcp）:api启动成功";
            emit apiReady();
        }
        socket->deleteLater();
    });
    connect(socket, &QAbstractSocket::errorOccurred, socket, &QObject::deleteLater);
    socket->connectToHost("127.0.0.1", m_port);
}

void TTSProcessManager::fail(const QString &reason)
{
    apiStop();
    emit apiFailed(reason);
}
