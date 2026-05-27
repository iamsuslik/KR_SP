#include "TelemetryManager.h"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <map>
#include <shared_mutex>

/**
 * Внутренний метод очистки. Удаляет записи старше 10 минут (600 секунд).
 * Вызывается только под Write Lock.
 */
void TelemetryManager::cleanup_unsafe() {
    auto now = std::chrono::steady_clock::now();
    const auto limit = std::chrono::seconds(600);

    while (!history.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - history.front().timestamp);
        if (age > limit) {
            history.pop_front();
        } else {
            break; // Остальные данные еще актуальны (deque упорядочен по времени)
        }
    }
}

void TelemetryManager::recordQuery(long long duration_ms, bool is_error) {
    // Используем уникальную блокировку (Write Lock) для изменения истории
    std::unique_lock lock(mtx);
    history.push_back({std::chrono::steady_clock::now(), duration_ms, is_error});
    cleanup_unsafe();
}

double TelemetryManager::getCurrentRPS(int seconds) const {
    // Используем разделяемую блокировку (Read Lock)
    std::shared_lock lock(mtx);
    if (history.empty()) return 0.0;

    auto now = std::chrono::steady_clock::now();
    auto count = std::count_if(history.begin(), history.end(), [&](const auto& m) {
        return std::chrono::duration_cast<std::chrono::seconds>(now - m.timestamp).count() < 1;
    });
    return static_cast<double>(count);
}

double TelemetryManager::getAvgRPS(int minutes) const {
    std::shared_lock lock(mtx);
    if (history.empty()) return 0.0;

    // Считаем фактический интервал времени в истории (но не более заданных минут)
    auto now = std::chrono::steady_clock::now();
    auto oldest = history.front().timestamp;
    auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - oldest).count();
    
    if (duration_sec <= 0) return static_cast<double>(history.size());
    
    double target_period = std::min(static_cast<double>(duration_sec), static_cast<double>(minutes * 60));
    return static_cast<double>(history.size()) / target_period;
}

double TelemetryManager::getMaxRPS(int minutes) const {
    std::shared_lock lock(mtx);
    if (history.empty()) return 0.0;

    // Группируем запросы по абсолютным секундам для поиска пиковой нагрузки
    std::map<long long, int> buckets;
    auto now = std::chrono::steady_clock::now();

    for (const auto& m : history) {
        auto sec_ago = std::chrono::duration_cast<std::chrono::seconds>(now - m.timestamp).count();
        if (sec_ago < (minutes * 60)) {
            buckets[sec_ago]++;
        }
    }

    int max_count = 0;
    for (auto const& [sec, count] : buckets) {
        if (count > max_count) max_count = count;
    }
    return static_cast<double>(max_count);
}

double TelemetryManager::getErrorRate(int minutes) const {
    std::shared_lock lock(mtx);
    if (history.empty()) return 0.0;

    auto now = std::chrono::steady_clock::now();
    int total = 0;
    int errors = 0;

    for (const auto& m : history) {
        if (std::chrono::duration_cast<std::chrono::minutes>(now - m.timestamp).count() < minutes) {
            total++;
            if (m.is_error) errors++;
        }
    }

    if (total == 0) return 0.0;
    return (static_cast<double>(errors) / total) * 100.0;
}

double TelemetryManager::getAvgDuration(int seconds) const {
    std::shared_lock lock(mtx);
    if (history.empty()) return 0.0;

    auto now = std::chrono::steady_clock::now();
    long long total_ms = 0;
    int count = 0;

    for (const auto& m : history) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - m.timestamp).count() < seconds) {
            total_ms += m.duration_ms;
            count++;
        }
    }

    if (count == 0) return 0.0;
    return static_cast<double>(total_ms) / count;
}

void TelemetryManager::printMetrics() const {
    std::shared_lock lock(mtx);
    
    std::cout << "\n" << std::string(REPORT_WIDTH, '=') << "\n"
              << "   DBMS REAL-TIME PERFORMANCE REPORT\n"
              << std::string(REPORT_WIDTH, '-') << "\n"
              
              << std::left << std::setw(LABEL_COLUMN_WIDTH) 
              << "Current RPS (1s):" << getCurrentRPS() << "\n"
              
              << std::left << std::setw(LABEL_COLUMN_WIDTH) 
              << "Average RPS (" << METRIC_RPS_WINDOW_MIN << "m):" 
              << std::fixed << std::setprecision(FLOAT_PRECISION_RPS) 
              << getAvgRPS(METRIC_RPS_WINDOW_MIN) << "\n"
              
              << std::left << std::setw(LABEL_COLUMN_WIDTH) 
              << "Peak RPS (" << METRIC_RPS_WINDOW_MIN << "m):" 
              << getMaxRPS(METRIC_RPS_WINDOW_MIN) << "\n"
              
              << std::left << std::setw(LABEL_COLUMN_WIDTH) 
              << "Avg Latency (" << METRIC_LATENCY_WINDOW_SEC << "s):" 
              << getAvgDuration(METRIC_LATENCY_WINDOW_SEC) << " ms\n"
              
              << std::left << std::setw(LABEL_COLUMN_WIDTH) 
              << "Error Rate (" << METRIC_ERROR_WINDOW_MIN << "m):" 
              << std::fixed << std::setprecision(FLOAT_PRECISION_ERROR) 
              << getErrorRate(METRIC_ERROR_WINDOW_MIN) << "%\n"
              
              << std::string(REPORT_WIDTH, '=') << std::endl;
}