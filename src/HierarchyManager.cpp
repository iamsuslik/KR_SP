#include "HierarchyManager.h"
#include <filesystem>
#include <iostream>
#include <fstream>

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

Result HierarchyManager::prepareTablePath(const std::string& table_name, std::string& out_path) const 
{
    if (current_db.empty()) {
        return {false, "Error: No database selected."};
    }

    fs::path table_path = fs::path(ROOT_DIR) / current_db / (table_name + ".db");

    if (fs::exists(table_path)) {
        return {false, "Error: Table '" + table_name + "' already exists."};
    }

    out_path = table_path.string();
    return {true, "Path prepared"};
}

std::string HierarchyManager::getCurrentDB() const {
    return current_db;
}
