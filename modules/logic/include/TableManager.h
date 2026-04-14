#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include "../../shared/include/common.h"
#include "../../core/include/Pager.h"
#include <vector>
#include <memory>

class TableManager {
public:
    static Result createTable(const std::string& full_path, 
                             const TableSchema& schema);

    static Result insertRow(const std::string& full_path, 
                           const Row& row);
private:
    static Result serializeRow(const Row& input_row, char* out_slot, const TableHeader& header);
};

#endif // TABLE_MANAGER_H
