#pragma once
#include <QString>

namespace RuntimeLog {
// Read the logging section before ConfigManager emits its own startup diagnostics.
void start(const QString &appData);
bool configure(const QString &directory, bool separateRuns, QString *error = nullptr);
void stop();
}
