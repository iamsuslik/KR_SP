#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include "../../shared/include/common.h"
#include "../../core/include/Pager.h"
#include <vector>
#include <memory>
#include <map>

class TableManager {
public:
    static Result createTable(const std::string& full_path, 
                             const TableSchema& schema);

    static Result insertRow(const std::string& full_path, const Row& row);

    static Result executeUpdate(const std::string& full_path, 
                           const Condition& cond, 
                           const std::string& targetCol, 
                           const std::string& newVal);

    static Result dropTable(const std::string& full_path);

    static Result showAllRows(const std::string& full_path, const std::string& table_name);

    // static Result deleteRow(const std::string& full_path, uint32_t p_id, uint32_t slot_id);

    static Result updateRow(const std::string& full_path, uint32_t p_id, uint32_t slot_id, const Row& new_row);

    static Result selectRows(const std::string& full_path, const Condition& cond);

    static Result deleteRows(const std::string& full_path, const Condition& cond);
    static Result executeDelete(const std::string& full_path, const Condition& cond);
    static Result executeSelect(const std::string& full_path, 
                           const Condition& cond, 
                           const std::map<std::string, std::string>& aliases = {});
    static bool matches(const Row& row, const TableHeader& header, const Condition& cond);


private:
    static Result serializeRow(const Row& input_row, char* out_slot, const TableHeader& header);
};

#endif // TABLE_MANAGER_H
