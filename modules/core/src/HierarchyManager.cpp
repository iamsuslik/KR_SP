#include "HierarchyManager.h"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

HierarchyManager::HierarchyManager() {
    if (!std::filesystem::exists(ROOT_DIR)) {
        try {
            std::filesystem::create_directories(ROOT_DIR);
        } catch (...) {
        }
    }
}

Result HierarchyManager::createDatabase(const std::string& db_name) {
    std::filesystem::path db_path = std::filesystem::path(ROOT_DIR) / db_name;

    if (std::filesystem::exists(db_path)) {
        return Result::Error(StatusCode::ALREADY_EXISTS, "Database '" + db_name + "' already exists.");
    }

    try {
        if (std::filesystem::create_directories(db_path)) {
            Result res = Result::Success();
            res.details = "Database '" + db_name + "' created successfully.";
            return res;
        }
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::IO_ERROR, e.what());
    }

    return Result::Error(StatusCode::INTERNAL_ERROR, "Failed to create database directory.");
}

Result HierarchyManager::dropDatabase(const std::string& db_name) {
    std::filesystem::path db_path = std::filesystem::path(ROOT_DIR) / db_name;

    // Логика: если папки нет — удалять нечего
    if (!std::filesystem::exists(db_path)) {
        // ИСПРАВЛЕНО: используем код DATABASE_NOT_FOUND (Грех №7)
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "Error: Database '" + db_name + "' does not exist.");
    }

    // Логика: если удаляем ту базу, в которой сейчас находимся — сбрасываем контекст
    if (current_db == db_name) {
        current_db = "";
    }

    try {
        // Физическое удаление папки со всеми таблицами внутри
        std::filesystem::remove_all(db_path);

        Result res = Result::Success();
        res.details = "Database '" + db_name + "' dropped.";
        return res;
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::IO_ERROR, e.what());
    }
}

Result HierarchyManager::useDatabase(const std::string& db_name) {
    std::filesystem::path db_path = std::filesystem::path(ROOT_DIR) / db_name;

    // Логика: если папки базы данных нет на диске — выдать ошибку
    if (!std::filesystem::exists(db_path)) {
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "Error: Database '" + db_name + "' does not exist.");
    }

    // Логика: переключаем контекст выполнения на выбранную базу
    current_db = db_name;

    Result res = Result::Success();
    res.details = "Database changed to '" + db_name + "'.";
    return res;
}

std::string HierarchyManager::getCurrentDB() const {
    return current_db;
}

Result HierarchyManager::resolveTablePath(const std::string& input_name) const {
    std::string target_db = current_db;
    std::string table_name = input_name;

    // Логика: если в имени есть точка (db.table), разделяем их
    size_t dot_pos = input_name.find('.');
    if (dot_pos != std::string::npos) {
        target_db = input_name.substr(0, dot_pos);
        table_name = input_name.substr(dot_pos + 1);
    }

    // Логика: если база не выбрана и не указана через точку — это ошибка
    if (target_db.empty()) {
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "No database selected and no prefix provided.");
    }

    std::filesystem::path db_folder = std::filesystem::path(ROOT_DIR) / target_db;
    
    // Проверка физического наличия папки базы данных
    if (!std::filesystem::exists(db_folder)) {
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "Database '" + target_db + "' not found.");
    }

    // Формируем полный путь к файлу таблицы
    std::string full_path = (db_folder / (table_name + ".db")).string();
    bool exists = std::filesystem::exists(full_path);

    //  не возвращаем магические строки "EXIST"/"NEW".
    // Мы используем StatusCode, чтобы SQLParser мог легко принимать решение.
    Result res;
    res.path = full_path;
    if (exists) {
        res.code = StatusCode::OK;        // Таблица найдена, можно открывать (аналог EXIST)
        res.details = "EXIST"; 
    } else {
        res.code = StatusCode::NOT_FOUND; // Таблицы нет, но путь для создания готов (аналог NEW)
        res.details = "NEW";
    }
    
    return res;
}
