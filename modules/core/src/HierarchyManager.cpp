#include "../include/HierarchyManager.h"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

HierarchyManager::HierarchyManager() {
    if (!fs::exists(ROOT_DIR)) {
        fs::create_directory(ROOT_DIR);
        std::cout << "[System] Root directory '" << ROOT_DIR << "' created.\n";
    }
}

Result HierarchyManager::createDatabase(const std::string& db_name) {
    fs::path db_path = fs::path(ROOT_DIR) / db_name;
    if (fs::exists(db_path)) {
        return {false, "Error: Database '" + db_name + "' already exists."};
    }
    if (fs::create_directory(db_path)) {
        return {true, "Database '" + db_name + "' created successfully."};
    }
    return {false, "Fatal Error: Failed to create database directory."};
}

Result HierarchyManager::dropDatabase(const std::string& db_name) {
    fs::path db_path = fs::path(ROOT_DIR) / db_name;
    if (!fs::exists(db_path)) {
        return {false, "Error: Database '" + db_name + "' does not exist."};
    }
    if (current_db == db_name) current_db = "";
    fs::remove_all(db_path);
    return {true, "Database '" + db_name + "' dropped."};
}

Result HierarchyManager::useDatabase(const std::string& db_name) {
    fs::path db_path = fs::path(ROOT_DIR) / db_name;
    if (!fs::exists(db_path)) {
        return {false, "Error: Database '" + db_name + "' does not exist."};
    }
    current_db = db_name;
    return {true, "Database changed to '" + db_name + "'."};
}

std::string HierarchyManager::getCurrentDB() const {
    return current_db;
}

Result HierarchyManager::resolveTablePath(const std::string& input_name) {
    std::string target_db = current_db;
    std::string table_name = input_name;

    size_t dot_pos = input_name.find('.');
    if (dot_pos != std::string::npos) {
        target_db = input_name.substr(0, dot_pos);
        table_name = input_name.substr(dot_pos + 1);
    }

    if (target_db.empty()) {
        return {false, "Error: No database selected and no database prefix provided."};
    }

    fs::path db_folder = fs::path(ROOT_DIR) / target_db;
    if (!fs::exists(db_folder)) {
        return {false, "Error: Database '" + target_db + "' not found."};
    }

    std::string full_path = (db_folder / (table_name + ".db")).string();
    bool exists = fs::exists(full_path);

    Result res;
    res.success = true;
    res.message = exists ? "EXIST" : "NEW";
    res.path = full_path;
    return res;
}