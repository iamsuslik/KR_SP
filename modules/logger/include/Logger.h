#ifndef LOGGER_H
#define LOGGER_H

#include <string>

class Logger {
public:
    static void log(const std::string& query, const std::string& status, long long duration_ms);
};

#endif