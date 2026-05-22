#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <string>
#include <vector>
#include <functional>
#include "common.h"
#include "HierarchyManager.h"
#include "TableManager.h"
#include <stack>

class SQLParser {
public:
    // Псевдоним для удобства
    using OutputCallback = std::function<void(const std::string&)>;

    // Главный метод теперь принимает колбэк
    Result process(const std::string& query, HierarchyManager& hm, OutputCallback callback);

private:
    int findColumnIndex(const TableHeader& header, const std::string& colName) const;
    bool applyConstraints(Row& row, const TableHeader& header, OutputCallback callback) const;
    size_t findKeyword(const std::vector<std::string>& tokens, const std::string& keyword);
    Result parseProjection(const std::vector<std::string>& tokens, size_t fromIdx, 
                       std::vector<std::string>& selectedCols, 
                       std::vector<AggregateRequest>& aggs, 
                       std::map<std::string, std::string>& aliases);
    Result parseColumnDefinitions(const std::vector<std::string>& tokens, size_t& index, std::vector<ColumnDef>& outCols);
    void applyOperator(std::stack<std::shared_ptr<ExpressionNode>>& values, std::stack<std::string>& ops);
    Result validateTokenCase(const std::vector<std::string>& tokens, OutputCallback callback);
    Result handleDrop(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    std::shared_ptr<ExpressionNode> createLeaf(const std::string& token);
    // Все обработчики теперь тоже принимают колбэк
    Result handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    Result handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);

    // Вспомогательные методы (остаются без изменений)
    Value parseLiteral(const std::string& token) const;
    std::vector<std::string> tokenize(const std::string& query) const;
    static std::string toUpper(std::string s);
    bool isValidIdentifier(const std::string& name);
    int getPrecedence(const std::string& op);
    std::shared_ptr<ExpressionNode> buildExpressionTree(const std::vector<std::string>& tokens);
    std::vector<std::string> getWhereTokens(const std::vector<std::string>& tokens) const;
    size_t findValueStartIndex(const std::vector<std::string>& tokens, std::vector<std::string>& outColNames) const;
    std::vector<std::string> collectValuesFromTokens(const std::vector<std::string>& tokens, size_t startIdx) const;
    bool prepareAndValidateRow(Row& outRow, const TableHeader& header, const std::vector<std::string>& targetCols, const std::vector<std::string>& rawValues, OutputCallback callback);
};

#endif
