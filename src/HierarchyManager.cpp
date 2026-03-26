#include "HierarchyManager.h"
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
    
    if (current_db == db_name) {
        current_db = "";
    }
    
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
