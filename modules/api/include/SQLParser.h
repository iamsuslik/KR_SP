#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <string>
#include <vector>
#include <functional>
#include "common.h"
#include "HierarchyManager.h"
#include "TableManager.h"

class SQLParser {
public:
    // Псевдоним для удобства
    using OutputCallback = std::function<void(const std::string&)>;

    // Главный метод теперь принимает колбэк
    void process(const std::string& query, HierarchyManager& hm, OutputCallback callback);

private:
    // Все обработчики теперь тоже принимают колбэк
    void handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    void handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    void handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    void handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    void handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    void handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);
    void handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback);

    // Вспомогательные методы (остаются без изменений)
    Value parseLiteral(const std::string& token);
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
