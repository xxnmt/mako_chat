#include <QApplication>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include "appcontroller.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "memoryembedding.h"

int main(int argc, char *argv[]) {
    //不创建应用名会报错
    QCoreApplication::setApplicationName("Windows_AI");
    // QCoreApplication::setOrganizationName("MakoDesktop");

    QApplication a(argc, argv);
    if (a.arguments().contains(QStringLiteral("--memory-index-status"))) {
        MemoryManager memory(ConfigManager::instance().getMemoryPath());
        qInfo().noquote() << QJsonDocument(memory.searchIndexStatus())
                                 .toJson(QJsonDocument::Indented);
        return 0;
    }
    if (a.arguments().contains(QStringLiteral("--backup-memory-db"))) {
        const QString source = ConfigManager::instance().getMemoryPath();
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const QString destination = QFileInfo(source).dir().filePath(
            QStringLiteral("QianDaoMoZi_memory.backup-%1.db").arg(stamp));
        MemoryManager memory(source);
        QString error;
        if (!memory.backupDatabase(destination, &error)) {
            qCritical() << "[MemoryBackup] failed:" << error;
            return 2;
        }
        qInfo() << "[MemoryBackup] wrote:" << destination;
        return 0;
    }
    if (a.arguments().contains(QStringLiteral("--rebuild-memory-index"))) {
        MemoryManager memory(ConfigManager::instance().getMemoryPath());
        QString error;
        if (!memory.rebuildSearchIndex(&error)) {
            qCritical() << "[MemoryIndex] rebuild failed:" << error;
            return 2;
        }
        const auto retrieval = ConfigManager::instance().getMemoryPolicyConfig().retrieval;
        if (retrieval.embeddingEnabled
            && retrieval.embeddingProvider == QStringLiteral("local_hash_test")) {
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 64;
            LocalHashEmbeddingProvider provider(dimension);
            if (!memory.embedSearchChunks(&provider, 2000, &error)) {
                qCritical() << "[MemoryIndex] embedding failed:" << error;
                return 2;
            }
        }
        qInfo() << "[MemoryIndex] rebuilt chunks:" << memory.memoryChunkCount();
        return 0;
    }
    if (a.arguments().contains(QStringLiteral("--clear-memory-index"))) {
        MemoryManager memory(ConfigManager::instance().getMemoryPath());
        QString error;
        if (!memory.clearSearchIndex(&error)) {
            qCritical() << "[MemoryIndex] clear failed:" << error;
            return 2;
        }
        qInfo() << "[MemoryIndex] cleared";
        return 0;
    }
    AppController ctrl;
    ctrl.startApp();

    return a.exec();
}