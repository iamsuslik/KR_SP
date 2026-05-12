#include "../include/Logger.h"
#include <fstream>
#include <ctime>
#include <iomanip>

void Logger::log(const std::string& query, const std::string& status, long long duration) {
    std::ofstream logFile("logs/access.log", std::ios::app);

    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

    logFile << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] "
            << "[ClientID: 1] "
            << "[HandlerID: 1] "
            << "[Query: " << query << "] "
            << "[Status: " << status << "] "
            << "[Duration: " << duration << "ms]\n";
}
