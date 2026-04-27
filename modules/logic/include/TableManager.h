#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include "../../shared/include/common.h"
#include "../../core/include/Pager.h"
#include <vector>
#include <string>
#include <map>

class TableManager {
public:
    static Result createTable(const std::string& full_path, const TableSchema& schema);
    static Result insertRow(const std::string& full_path, const Row& row);

    static Result executeSelect(const std::string& full_path, 
                           const Condition& cond, 
                           const std::vector<std::string>& selectedCols = {},
                           const std::map<std::string, std::string>& aliases = {});
    
    static Result executeUpdate(const std::string& full_path, 
                               const Condition& cond, 
                               const std::string& targetCol, 
                               const std::string& newVal);

    static Result executeDelete(const std::string& full_path, const Condition& cond);

    static Result dropTable(const std::string& full_path);

private:
    static Result serializeRow(const Row& input_row, char* out_slot, const TableHeader& header);
    static bool matches(const Row& row, const TableHeader& header, const Condition& cond);
};

#endif // TABLE_MANAGER_H
