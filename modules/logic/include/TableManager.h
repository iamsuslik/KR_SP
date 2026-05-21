#ifndef TABLE_MANAGER_H
#define TABLE_MANAGER_H

#include "common.h"
#include "Pager.h"
#include "RecordManager.h"
#include <vector>
#include <string>
#include <map>

class TableManager {
public:
    // Основные методы выполнения SQL-команд
    static Result executeSelect(const std::string& full_path,
                               const ExpressionNode* cond,
                               const std::vector<std::string>& selectedCols = {},
                               const std::map<std::string, std::string>& aliases = {},
                               const std::vector<AggregateRequest>& aggs = {});

    static Result createTable(const std::string& full_path, const TableSchema& schema);

    static Result insertRow(const std::string& full_path, const Row& row);

    static Result executeUpdate(const std::string& full_path,
                               const ExpressionNode* cond,
                               const std::string& targetCol,
                               const std::string& newVal);

    static Result executeDelete(const std::string& full_path, const ExpressionNode* cond);

    static Result dropTable(const std::string& full_path);

    // Логика сопоставления условий (WHERE)
    static bool matches(const Row& row, const TableHeader& header, const ExpressionNode* node);
    static bool evaluateLeaf(const Row& row, const TableHeader& header, const ExpressionNode* cond);

private:
    // Вспомогательные методы вывода и агрегации
    static void printRowAsJson(const Row& row, const TableHeader& header,
                               const std::vector<uint32_t>& colsToPrint,
                               const std::map<std::string, std::string>& aliases,
                               bool& isFirst);

    static void applyAggregates(const Row& row, const TableHeader& header,
                                const std::vector<AggregateRequest>& aggs,
                                long long& total_sum, int& total_count);

    static std::vector<uint32_t> getProjection(const TableHeader& header, const std::vector<std::string>& selectedCols);

    static void processRow(const Row& row, const TableHeader& header, const ExpressionNode* cond,
                           const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint,
                           const std::map<std::string, std::string>& aliases,
                           long long& t_sum, int& t_count, bool& first, bool isAgg);

    static void renderAggregates(const std::vector<AggregateRequest>& aggs, long long t_sum, int t_count);

    // Управление физическим расположением и индексами
    static RecordID findAvailableSlot(Pager& pager, TableHeader& header);
    static void updateIndices(Pager& pager, TableHeader& header, const Row& row, const RecordID& rid);
    static void updateFieldAndIndex(Row& row, uint32_t colIdx, const std::string& newVal,
                                   TableHeader& header, Pager& pager, RecordID rid, bool& header_changed);

    // Логика работы с B+ деревом
    static Result getRIDFromIndex(Pager& pager, TableHeader& header, const ExpressionNode* cond, RecordID& out_rid);
    
    static bool executePointQuery(Pager& pager, TableHeader& header, const ExpressionNode* cond,
                             const std::vector<uint32_t>& colsToPrint, const std::map<std::string, std::string>& aliases,
                             const std::vector<AggregateRequest>& aggs, long long& t_sum, int& t_count, bool& first);

    static void executeTreeScan(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                               const std::vector<uint32_t>& colsToPrint, 
                               const std::map<std::string, std::string>& aliases,
                               const std::vector<AggregateRequest>& aggs, 
                               long long& t_sum, int& t_count, bool& first, bool isAgg);
    static int fullScanDelete(Pager& pager, TableHeader& header, const ExpressionNode* cond);
    static int fullScanUpdate(Pager& pager, TableHeader& header, const ExpressionNode* cond, 
                              const std::string& targetCol, const std::string& newVal);
    static void clearIndicesForRow(Pager& pager, TableHeader& header, const Row& row);
    static void fullScanSelect(Pager& pager, TableHeader& header, const ExpressionNode* cond,
                           const std::vector<AggregateRequest>& aggs, const std::vector<uint32_t>& colsToPrint,
                           const std::map<std::string, std::string>& aliases,
                           long long& t_sum, int& t_count, bool& first, bool isAgg);
};

#endif // TABLE_MANAGER_H
