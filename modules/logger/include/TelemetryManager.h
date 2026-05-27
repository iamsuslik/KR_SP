#ifndef TELEMETRY_MANAGER_H
#define TELEMETRY_MANAGER_H

#include <deque>
#include <chrono>
#include <mutex>
#include <shared_mutex>

struct QueryMetric {
    std::chrono::steady_clock::time_point timestamp;
    long long duration_ms;
    bool      is_error;
};

class TelemetryManager {
public:
    TelemetryManager() = default;

    void   recordQuery(long long duration_ms, bool is_error);
    void   printMetrics() const;

    double getCurrentRPS(int seconds = 1) const;
    double getAvgRPS    (int minutes  = 10) const;
    double getMaxRPS    (int minutes  = 10) const;
    double getAvgDuration(int seconds = 10) const;
    double getErrorRate (int minutes  = 1)  const;

private:
    std::deque<QueryMetric> history;
    mutable std::shared_mutex mtx;
    void cleanup_unsafe();
};

#endif