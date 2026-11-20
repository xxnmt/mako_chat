#pragma once
#include <QString>
#include <limits>

// Shared fallback reading/playback estimate. Real audio uses its actual duration.
inline int simulatedTextPlaybackMs(const QString &text)
{
    return int(qBound<qint64>(qint64(1000), qint64(text.size()) * 250,
                             qint64(std::numeric_limits<int>::max())));
}
