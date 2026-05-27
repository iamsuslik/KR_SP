#ifndef SYS_PROG_ERROR_UTILS_H
#define SYS_PROG_ERROR_UTILS_H

#include <string>
#include "common.h"
#include "DbException.h"

class ErrorUtils {
public:
    // Превращает код статуса в базовое текстовое описание
    static std::string statusCodeToString(StatusCode code);

    // Формирует полное сообщение об ошибке, включая детали из Result
    static std::string formatMessage(const Result& res);

    // Статический помощник для вывода ошибки в консоль (с цветом или префиксом)
    static void printError(const Result& res);
};

#endif // SYS_PROG_ERROR_UTILS_H