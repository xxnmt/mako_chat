#include <QApplication>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <memory>

#include "appcontroller.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "memoryembedding.h"
#include "memorydashscope.h"

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
    if (a.arguments().contains(QStringLiteral("--embed-memory-index"))) {
        MemoryManager memory(ConfigManager::instance().getMemoryPath());
        const auto retrieval = ConfigManager::instance().getMemoryPolicyConfig().retrieval;
        std::unique_ptr<MemoryEmbeddingProvider> provider;
        if (retrieval.embeddingProvider == QStringLiteral("local_hash_test")) {
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 64;
            provider = std::make_unique<LocalHashEmbeddingProvider>(dimension);
        } else if (retrieval.embeddingProvider == QStringLiteral("dashscope")) {
            const QString apiKey = !retrieval.embeddingApiKey.isEmpty()
                ? retrieval.embeddingApiKey
                : qEnvironmentVariable(retrieval.embeddingApiKeyEnv.toUtf8().constData());
            if (apiKey.isEmpty() || retrieval.embeddingEndpoint.isEmpty()) {
                qCritical() << "[MemoryIndex] DashScope endpoint or API key missing";
                return 2;
            }
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 1024;
            provider = std::make_unique<DashScopeEmbeddingProvider>(
                QUrl(retrieval.embeddingEndpoint), apiKey,
                retrieval.embeddingModel.isEmpty()
                    ? QStringLiteral("text-embedding-v4") : retrieval.embeddingModel,
                dimension);
        }
        if (!provider) {
            qCritical() << "[MemoryIndex] embedding provider not configured";
            return 2;
        }
        QString error;
        if (!memory.embedSearchChunks(provider.get(), 2000, &error)) {
            qCritical() << "[MemoryIndex] embedding failed:" << error;
            return 2;
        }
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
        std::unique_ptr<MemoryEmbeddingProvider> provider;
        if (retrieval.embeddingEnabled
            && retrieval.embeddingProvider == QStringLiteral("local_hash_test")) {
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 64;
            provider = std::make_unique<LocalHashEmbeddingProvider>(dimension);
        } else if (retrieval.embeddingEnabled
                   && retrieval.embeddingProvider == QStringLiteral("dashscope")) {
            const QString apiKey = !retrieval.embeddingApiKey.isEmpty()
                ? retrieval.embeddingApiKey
                : qEnvironmentVariable(retrieval.embeddingApiKeyEnv.toUtf8().constData());
            if (apiKey.isEmpty() || retrieval.embeddingEndpoint.isEmpty()) {
                qCritical() << "[MemoryIndex] DashScope endpoint or API key missing";
                return 2;
            }
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 1024;
            provider = std::make_unique<DashScopeEmbeddingProvider>(
                QUrl(retrieval.embeddingEndpoint), apiKey,
                retrieval.embeddingModel.isEmpty()
                    ? QStringLiteral("text-embedding-v4") : retrieval.embeddingModel,
                dimension);
        }
        if (provider && !memory.embedSearchChunks(provider.get(), 2000, &error)) {
            qCritical() << "[MemoryIndex] embedding failed:" << error;
            return 2;
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