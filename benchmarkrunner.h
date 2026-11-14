#ifndef BENCHMARKRUNNER_H
#define BENCHMARKRUNNER_H

#include <QStringList>

// 评测专用无头入口。只在显式 --bench-* 参数下运行，不启动 AppController/GUI。
int runBenchmarkImport(const QStringList &args);
int runBenchmarkAsk(const QStringList &args);
int runBenchmarkRetrieve(const QStringList &args);

#endif // BENCHMARKRUNNER_H
