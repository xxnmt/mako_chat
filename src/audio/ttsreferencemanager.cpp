#include "ttsreferencemanager.h"
#include "configmanager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>
#include <QDir>

bool TTSReferenceManager::loadMap()
{
    QString mapPath=ConfigManager::instance().getAppDataPath()+"/reference_audio/tts_map.json";
    QFile file(mapPath);
    if(!file.open(QIODevice::ReadOnly|QIODevice::Text)){
        qDebug()<<"[TTSRM]打开tts_map失败:"<<mapPath;
        return false;
    }
    QByteArray data=file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc=QJsonDocument::fromJson(data,&error);
    if(error.error != QJsonParseError::NoError){
        qDebug()<<"[TTSRM]ttsmap解析失败:"<<error.error;
        return false;
    }
    m_mapRoot=doc.object();
    qDebug()<<"[TTSRM]:ttsmap成功加载";
    return true;
}

TTSReference TTSReferenceManager::getReferenceForEmotion(const QString &emotion) const
{
    TTSReference ref;
    if (m_mapRoot.contains(emotion) && m_mapRoot[emotion].isObject()) {
        QJsonObject obj = m_mapRoot[emotion].toObject();
        ref.audioFilePath = obj.value("audio_file").toString();
        ref.promptText = obj.value("prompt_text").toString();
        ref.promptLanguage = obj.value("prompt_language").toString("ja");
    }
    else if (m_mapRoot.contains("happyIdle") && m_mapRoot["happyIdle"].isObject()) {
        QJsonObject obj = m_mapRoot["happyIdle"].toObject();
        ref.audioFilePath = obj.value("audio_file").toString();
        ref.promptText = obj.value("prompt_text").toString();
        ref.promptLanguage = obj.value("prompt_language").toString("ja");
    }

    QFileInfo fileInfo(ref.audioFilePath);
    if (!fileInfo.isAbsolute()) {
        ref.audioFilePath = ConfigManager::instance().getAppDataPath() + "/reference_audio/" + ref.audioFilePath;
    }
    return ref;
}

TTSReferenceManager::TTSReferenceManager()
{
    loadMap();
}


