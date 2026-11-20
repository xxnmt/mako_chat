#ifndef DEBUGLOG_H
#define DEBUGLOG_H

#include <atomic>

// 默认关闭。由 ConfigManager 从 setting.json 的
// memory.debug_full_logs 读取后设置。
inline std::atomic_bool g_debugFullLogsEnabled{false};

inline bool debugFullLogsEnabled()
{
    return g_debugFullLogsEnabled.load(std::memory_order_relaxed);
}

inline void setDebugFullLogsEnabled(bool enabled)
{
    g_debugFullLogsEnabled.store(enabled, std::memory_order_relaxed);
}

#endif // DEBUGLOG_H
