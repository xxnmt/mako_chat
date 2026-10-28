#ifndef DEBUGLOG_H
#define DEBUGLOG_H

// 默认关闭。由 ConfigManager 从 setting.json 的
// memory.debug_full_logs 读取后设置。
inline bool g_debugFullLogsEnabled = false;

inline bool debugFullLogsEnabled()
{
    return g_debugFullLogsEnabled;
}

inline void setDebugFullLogsEnabled(bool enabled)
{
    g_debugFullLogsEnabled = enabled;
}

#endif // DEBUGLOG_H
