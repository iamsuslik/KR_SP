#include "HierarchyManager.h"
#include <filesystem>

namespace fs = std::filesystem;

static fs::path node_root(int node_id) {
    if (node_id < 0)
        return fs::path(HierarchyManager::ROOT_DIR);
    return fs::path(HierarchyManager::ROOT_DIR) / ("node_" + std::to_string(node_id));
}

HierarchyManager::HierarchyManager() {
    if (!fs::exists(ROOT_DIR)) {
        try { fs::create_directories(ROOT_DIR); } catch (...) {}
    }
}

Result HierarchyManager::createDatabase(const std::string& db_name) {
    bool created_any = false;
    for (int n = -1; n < N_NODES; ++n) {
        fs::path db_path = node_root(n) / db_name;
        if (!fs::exists(db_path)) {
            try {
                fs::create_directories(db_path);
                created_any = true;
            } catch (const std::exception& e) {
                return Result::Error(StatusCode::IO_ERROR, e.what());
            }
        }
        if (n < 0) break;
    }

    if (!created_any)
        return Result::Error(StatusCode::ALREADY_EXISTS, "Database '" + db_name + "' already exists.");

    Result res = Result::Success();
    res.details = "Database '" + db_name + "' created successfully.";
    return res;
}

Result HierarchyManager::dropDatabase(const std::string& db_name) {
    bool found = false;
    for (int n = -1; n < N_NODES; ++n) {
        fs::path db_path = node_root(n) / db_name;
        if (fs::exists(db_path)) {
            try { fs::remove_all(db_path); found = true; }
            catch (const std::exception& e) {
                return Result::Error(StatusCode::IO_ERROR, e.what());
            }
        }
        if (n < 0) break;
    }

    if (!found) {
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "Database '" + db_name + "' does not exist.");
    }

    if (current_db == db_name) current_db.clear();

    Result res = Result::Success();
    res.details = "Database '" + db_name + "' dropped.";
    return res;
}

Result HierarchyManager::useDatabase(const std::string& db_name) {
    fs::path db_path = node_root(-1) / db_name;
    if (!fs::exists(db_path))
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "Database '" + db_name + "' does not exist.");

    current_db = db_name;

    Result res = Result::Success();
    res.details = "Database changed to '" + db_name + "'.";
    return res;
}

std::string HierarchyManager::getCurrentDB() const {
    return current_db;
}

Result HierarchyManager::resolveTablePath(const std::string& input_name, int node_id) const {
    std::string target_db, table_name;
    size_t dot = input_name.find('.');
    if (dot != std::string::npos) {
        target_db = input_name.substr(0, dot);
        table_name = input_name.substr(dot + 1);
    } else {
        target_db = current_db;
        table_name = input_name;
    }

    if (target_db.empty()) {
        return Result::Error(StatusCode::DATABASE_NOT_FOUND, "No database selected and no prefix provided.");
    }

    fs::path db_folder = node_root(node_id) / target_db;

    if (!fs::exists(db_folder)) {
        if (node_id >= 0) {
            try { fs::create_directories(db_folder); }
            catch (const std::exception& e) {
                return Result::Error(StatusCode::IO_ERROR, e.what());
            }
        } else {
            return Result::Error(StatusCode::DATABASE_NOT_FOUND, "Database '" + target_db + "' not found.");
        }
    }

    std::string full_path = (db_folder / (table_name + ".db")).string();
    bool exists = fs::exists(full_path);

    Result res;
    res.path = full_path;
    res.code = exists ? StatusCode::OK : StatusCode::NOT_FOUND;
    res.details = exists ? "EXIST" : "NEW";
    return res;
}
