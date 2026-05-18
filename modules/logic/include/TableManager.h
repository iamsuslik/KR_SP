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
                           const ExpressionNode* cond, 
                           const std::vector<std::string>& selectedCols = {},
                           const std::map<std::string, std::string>& aliases = {});
    
    static Result executeUpdate(const std::string& full_path, 
                               const ExpressionNode* cond, 
                               const std::string& targetCol, 
                               const std::string& newVal);

    static Result executeDelete(const std::string& full_path, const ExpressionNode* cond);

    static Result dropTable(const std::string& full_path);
    static bool matches(const Row& row, const TableHeader& header, const ExpressionNode* node);
    static bool evaluateLeaf(const Row& row, const TableHeader& header, const ExpressionNode* cond);

private:
    static Result serializeRow(const Row& input_row, char* out_slot, const TableHeader& header);

};

#endif // TABLE_MANAGER_H
