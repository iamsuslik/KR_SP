#ifndef HIERARCHY_MANAGER_H
#define HIERARCHY_MANAGER_H

#include <string>
#include "../../shared/include/common.h"

class HierarchyManager {
private:
    const std::string ROOT_DIR = "data";
    std::string current_db = "";

public:

    HierarchyManager();

    Result createDatabase(const std::string& db_name);
    Result dropDatabase(const std::string& db_name);
    Result useDatabase(const std::string& db_name);
    Result prepareTablePath(const std::string& table_name, std::string& out_path) const;

    std::string getCurrentDB() const;

    Result getPathForCreate(const std::string& table_name, std::string& out_path) const;

    Result getPathForExisting(const std::string& table_name, std::string& out_path) const;

    Result dropTable(const std::string& table_name);
};

#endif // HIERARCHY_MANAGER_H
