#ifndef TELEMETRY_MANAGER_H
#define TELEMETRY_MANAGER_H

#include <deque>
#include <chrono>
#include <mutex>
#include <shared_mutex> // Оптимизация для частого чтения метрик
#include "common.h"

// Структура единичного замера производительности
struct QueryMetric {
    std::chrono::steady_clock::time_point timestamp;
    long long duration_ms;
    bool is_error;
};

class TelemetryManager {
private:
    std::deque<QueryMetric> history;
    
    // mutable shared_mutex позволяет использовать блокировки внутри const методов
    mutable std::shared_mutex mtx; 
    
    // Внутренний метод очистки устаревших данных (старше 10 минут)
    // Вызывается только внутри методов, уже захвативших замок
    void cleanup_unsafe();

public:
    TelemetryManager() = default;
    ~TelemetryManager() = default;

    // Запись данных о завершенном запросе
    void recordQuery(long long duration_ms, bool is_error);

    // Вывод текущих показателей в консоль сервера
    void printMetrics() const;

    // --- Методы расчета аналитики (Thread-safe) ---
    
    double getCurrentRPS() const;          // RPS за последнюю секунду
    double getAvgRPS(int minutes = 10) const; // Средний RPS за период
    double getMaxRPS(int minutes = 10) const; // Максимальный RPS за период
    double getAvgDuration(int seconds = 10) const; // Ср. время обработки за период
    double getErrorRate(int minutes = 1) const;    // % ошибок за последнюю минуту
};

#endif