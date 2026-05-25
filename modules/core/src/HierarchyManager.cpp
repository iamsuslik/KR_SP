#include "HierarchyManager.h"
#include <filesystem>

namespace fs = std::filesystem;

HierarchyManager::HierarchyManager() {
    if (!fs::exists(ROOT_DIR)) {
        try { fs::create_directories(ROOT_DIR); } catch (...) {}
    }
}

Result HierarchyManager::createDatabase(const std::string& db_name) {
    fs::path db_path = fs::path(ROOT_DIR) / db_name;
    if (fs::exists(db_path))
        return Result::Error(StatusCode::ALREADY_EXISTS,
                             "Database '" + db_name + "' already exists.");
    try {
        if (fs::create_directories(db_path)) {
            Result res = Result::Success();
            res.details = "Database '" + db_name + "' created successfully.";
            return res;
        }
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::IO_ERROR, e.what());
    }
    return Result::Error(StatusCode::INTERNAL_ERROR,
                         "Failed to create database directory.");
}

Result HierarchyManager::dropDatabase(const std::string& db_name) {
    fs::path db_path = fs::path(ROOT_DIR) / db_name;
    if (!fs::exists(db_path))
        return Result::Error(StatusCode::DATABASE_NOT_FOUND,
                             "Database '" + db_name + "' does not exist.");
    {
        std::unique_lock lock(db_mtx);
        if (current_db == db_name) current_db.clear();
    }
    try {
        fs::remove_all(db_path);
        Result res = Result::Success();
        res.details = "Database '" + db_name + "' dropped.";
        return res;
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::IO_ERROR, e.what());
    }
}

Result HierarchyManager::useDatabase(const std::string& db_name) {
    fs::path db_path = fs::path(ROOT_DIR) / db_name;
    if (!fs::exists(db_path))
        return Result::Error(StatusCode::DATABASE_NOT_FOUND,
                             "Database '" + db_name + "' does not exist.");
    {
        std::unique_lock lock(db_mtx);
        current_db = db_name;
    }
    Result res = Result::Success();
    res.details = "Database changed to '" + db_name + "'.";
    return res;
}

std::string HierarchyManager::getCurrentDB() const {
    std::shared_lock lock(db_mtx);
    return current_db;
}

Result HierarchyManager::resolveTablePath(const std::string& input_name) const {
    std::string target_db, table_name;
    size_t dot = input_name.find('.');
    if (dot != std::string::npos) {
        target_db  = input_name.substr(0, dot);
        table_name = input_name.substr(dot + 1);
    } else {
        std::shared_lock lock(db_mtx);
        target_db  = current_db;
        table_name = input_name;
    }

    if (target_db.empty())
        return Result::Error(StatusCode::DATABASE_NOT_FOUND,
                             "No database selected and no prefix provided.");

    fs::path db_folder = fs::path(ROOT_DIR) / target_db;
    if (!fs::exists(db_folder))
        return Result::Error(StatusCode::DATABASE_NOT_FOUND,
                             "Database '" + target_db + "' not found.");

    std::string full_path = (db_folder / (table_name + ".db")).string();
    bool exists = fs::exists(full_path);

    Result res;
    res.path    = full_path;
    res.code    = exists ? StatusCode::OK : StatusCode::NOT_FOUND;
    res.details = exists ? "EXIST" : "NEW";
    return res;
}