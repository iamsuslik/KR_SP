#include "Logger.h"
#include <fstream>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <ctime>

// Мьютекс для предотвращения "перемешивания" строк от разных потоков в файле
static std::mutex log_file_mtx;

void Logger::log(const std::string& query, const std::string& status, long long duration) {
    std::lock_guard<std::mutex> lock(log_file_mtx);
    
    namespace fs = std::filesystem;
    const std::string log_dir = "logs";
    const std::string log_file = log_dir + "/access.log";

    try {
        if (!fs::exists(log_dir)) {
            fs::create_directories(log_dir);
        }

        std::ofstream out(log_file, std::ios::app);
        if (!out.is_open()) return;

        // Потокобезопасное получение времени
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm time_struct;
        localtime_r(&now, &time_struct);

        char time_str[20];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_struct);

        // Строгий формат записи
        out << "[" << time_str << "] "
            << "[Status: " << status << "] "
            << "[Latency: " << duration << "ms] "
            << "[Query: " << query << "]" << std::endl;

    } catch (...) {
        // Логгер не должен приводить к падению основной системы при ошибках ФС
    }
}