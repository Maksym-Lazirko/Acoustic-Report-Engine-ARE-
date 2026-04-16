#include "DebugSessionLog.h"
#include "AreDebugLogPath.h"
#include <chrono>
#include <fstream>
#include <mutex>

// #region agent log
static std::mutex gAreDbgMtx;

void areDbgLog (const char* hypothesisId, const char* location, const char* message, int dataA, int dataB)
{
    std::lock_guard<std::mutex> lock (gAreDbgMtx);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    std::ofstream out (ARE_DBG_LOG_FILE_PATH, std::ios::app | std::ios::binary);
    if (! out)
        return;

    out << "{\"sessionId\":\"98f05a\",\"hypothesisId\":\"" << hypothesisId << "\",\"location\":\""
        << location << "\",\"message\":\"" << message << "\",\"data\":{\"a\":" << dataA << ",\"b\":" << dataB
        << "},\"timestamp\":" << ms << "}\n";
}
// #endregion
