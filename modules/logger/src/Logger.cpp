#include "../include/Logger.h"
#include <fstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <filesystem>


void Logger::log(const std::string& query, const std::string& status, long long duration) {
    std::filesystem::create_directories("logs");
    std::ofstream logFile("logs/access.log", std::ios::app);
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

    logFile << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] "
            << "[Query: " << query << "] "
            << "[Status: " << status << "] "
            << "[Duration: " << duration << "ms]\n";
}
