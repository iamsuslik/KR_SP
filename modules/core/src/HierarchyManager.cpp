#include "../include/HierarchyManager.h"
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

std::string HierarchyManager::getCurrentDB() const {
    return current_db;
}

Result HierarchyManager::getPathForCreate(const std::string& table_name, std::string& out_path) const {
    if (current_db.empty()) return {false, "Error: No database selected."};
    
    fs::path table_path = fs::path(ROOT_DIR) / current_db / (table_name + ".db");
    
    if (fs::exists(table_path)) {
        return {false, "Error: Table '" + table_name + "' already exists."};
    }
    
    out_path = table_path.string();
    return {true, "Path prepared for creation."};
}

Result HierarchyManager::getPathForExisting(const std::string& table_name, std::string& out_path) const {
    if (current_db.empty()) return {false, "Error: No database selected."};
    
    fs::path table_path = fs::path(ROOT_DIR) / current_db / (table_name + ".db");
    
    if (!fs::exists(table_path)) {
        return {false, "Error: Table '" + table_name + "' does not exist."};
    }
    
    out_path = table_path.string();
    return {true, "Path found."};
}

Result HierarchyManager::dropTable(const std::string& table_name) {
    if (current_db.empty()) return {false, "Error: No database selected."};

    fs::path table_path = fs::path(ROOT_DIR) / current_db / (table_name + ".db");
    
    if (!fs::exists(table_path)) {
        return {false, "Error: Table '" + table_name + "' does not exist."};
    }

    fs::remove(table_path);
    return {true, "Table '" + table_name + "' successfully dropped."};
}

Result HierarchyManager::prepareTablePath(const std::string& input_name, std::string& out_path) const {
    std::string db_to_use = current_db;
    std::string table_name = input_name;

    size_t dot_pos = input_name.find('.');
    
    if (dot_pos != std::string::npos) {
        db_to_use = input_name.substr(0, dot_pos);
        table_name = input_name.substr(dot_pos + 1);
    }

    if (db_to_use.empty()) {
        return {false, "Error: No database selected and no database prefix provided.", {0,0}};
    }

    fs::path db_folder = fs::path(ROOT_DIR) / db_to_use;
    if (!fs::exists(db_folder)) {
        return {false, "Error: Database '" + db_to_use + "' not found.", {0,0}};
    }

    out_path = (db_folder / (table_name + ".data")).string();
    
    return {true, "Path successfully resolved", {0,0}};
}
