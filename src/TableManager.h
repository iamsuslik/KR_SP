#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include "common.h"
#include "Pager.h"
#include <vector>
#include <memory>

class TableManager {
public:
    static Result createTable(const std::string& full_path, 
                             const TableSchema& schema);

    static Result insertRow(const std::string& full_path, 
                           const Row& row);
private:
    static void serializeRow(const Row& row, char* out_slot, uint32_t row_size);
};

#endif // TABLE_MANAGER_H
