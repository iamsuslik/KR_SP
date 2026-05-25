#ifndef HIERARCHY_MANAGER_H
#define HIERARCHY_MANAGER_H

#include <string>
#include <shared_mutex>
#include "common.h"

class HierarchyManager {
private:
    const std::string ROOT_DIR = "data";
    std::string current_db;
    mutable std::shared_mutex db_mtx;

public:
    HierarchyManager();

    Result createDatabase(const std::string& db_name);
    Result dropDatabase(const std::string& db_name);
    Result useDatabase(const std::string& db_name);

    std::string getCurrentDB() const;
    Result resolveTablePath(const std::string& input_name) const;
};

#endif // HIERARCHY_MANAGER_H