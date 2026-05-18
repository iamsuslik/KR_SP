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
                           const std::map<std::string, std::string>& aliases = {},
                           const std::vector<AggregateRequest>& aggs = {});
    
    static Result executeUpdate(const std::string& full_path, 
                               const Condition& cond, 
                               const std::string& targetCol, 
                               const std::string& newVal);

    static Result executeDelete(const std::string& full_path, const Condition& cond);

    static Result dropTable(const std::string& full_path);

private:
    static void initColumnSchema(ColumnSchema& dest, const ColumnDef& src);
    static void writeField(char* out_slot, int& offset, const Value* val, const ColumnSchema& col);
    static Result serializeRow(const Row& input_row, char* out_slot, const TableHeader& header);
    static bool matches(const Row& row, const TableHeader& header, const Condition& cond);
    static Row extractRow(char* slot_ptr, const TableHeader& header);
    static void printRowAsJson(const Row& row, const TableHeader& header, 
                               const std::vector<uint32_t>& colsToPrint, 
                               const std::map<std::string, std::string>& aliases, 
                               bool& isFirst);
    static void applyAggregates(const Row& row, const TableHeader& header, 
                                const std::vector<AggregateRequest>& aggs, 
                                long long& total_sum, int& total_count);
    
    static std::vector<uint32_t> getProjection(const TableHeader& header, const std::vector<std::string>& selectedCols);
    
    static void processRow(const Row& row, const TableHeader& header, const Condition& cond, 
                           const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint, 
                           const std::map<std::string, std::string>& aliases, 
                           long long& t_sum, int& t_count, bool& first, bool isAgg);

    static void renderAggregates(const std::vector<AggregateRequest>& aggs, long long t_sum, int t_count);
    static RecordID findAvailableSlot(Pager& pager, TableHeader& header);
    static void updateIndices(Pager& pager, TableHeader& header, const Row& row, const RecordID& rid);
    static bool isPageEmpty(char* page_buffer, uint32_t row_size);
    static void updateFieldAndIndex(Row& row, uint32_t colIdx, const std::string& newVal, 
                                   TableHeader& header, Pager& pager, RecordID rid, bool& header_changed);
};

#endif // TABLE_MANAGER_H
