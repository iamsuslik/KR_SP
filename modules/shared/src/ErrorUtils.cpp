#include "ErrorUtils.h"
#include <iostream>

std::string ErrorUtils::statusCodeToString(StatusCode code) {
    switch (code) {
        case StatusCode::OK:                 return "Success";
        case StatusCode::IO_ERROR:           return "System I/O Error";
        case StatusCode::NOT_FOUND:          return "Entity not found";
        case StatusCode::ALREADY_EXISTS:     return "Entity already exists";
        case StatusCode::OUT_OF_MEMORY:      return "Memory allocation failed";
        case StatusCode::TABLE_NOT_FOUND:    return "Table not found";
        case StatusCode::DATABASE_NOT_FOUND: return "Database not found";
        case StatusCode::COLUMN_NOT_FOUND:   return "Column not found";
        case StatusCode::TYPE_MISMATCH:      return "Data type mismatch";
        case StatusCode::DUPLICATE_KEY:      return "Unique constraint violation (Duplicate key)";
        case StatusCode::NOT_NULL_VIOLATION: return "NOT NULL constraint violation";
        case StatusCode::INVALID_VALUE:      return "Invalid value format";
        case StatusCode::SYNTAX_ERROR:       return "SQL Syntax error";
        case StatusCode::INTERNAL_ERROR:     return "Internal DBMS error";
        default:                             return "Unknown error code";
    }
}

std::string ErrorUtils::formatMessage(const Result& res) {
    if (res.isOk()) return "OK";

    std::string base = statusCodeToString(res.code);
    if (!res.details.empty()) {
        base += ": " + res.details;
    }
    return base;
}

void ErrorUtils::printError(const Result& res) {
    if (res.isOk()) return;
    
    // Вывод в поток ошибок cerr с понятным префиксом
    std::cerr << "[DBMS Error] " << formatMessage(res) << std::endl;
}