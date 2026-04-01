#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include "common.h"
#include "Pager.h"
#include <vector>
#include <memory>

class TableManager {
public:
    static Result createTable(const std::string& db_path, 
                             const std::string& table_name, 
                             const std::vector<ColumnDef>& columns);

    static Result showSchema(const std::string& db_path,
                            const std::string& table_name);
    
    static Result dropTable(const std::string& db_path,
                            const std::string& table_name);

    static Result insertRow(const std::string& db_path, 
                           const std::string& table_name, 
                           const Row& row);
private:
    static void serializeRow(const Row& row, char* out_slot);
};

#endif // TABLE_MANAGER_H
