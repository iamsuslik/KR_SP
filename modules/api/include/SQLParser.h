#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <iostream>
#include <algorithm>

#include "../../shared/include/common.h"
#include "../../core/include/HierarchyManager.h"
#include "../../logic/include/TableManager.h"

class SQLParser {
public:
    // Главный метод обработки строки запроса
    void process(std::string query, HierarchyManager& hm);

private:
    // Вспомогательные методы парсинга
    std::vector<std::string> tokenize(const std::string& query);
    bool isValidCase(const std::string& token);
    bool isValidIdentifier(const std::string& name);
    static std::string toUpper(std::string s);

    // Методы для обработки сложных условий WHERE
    int getPrecedence(const std::string& op);
    std::shared_ptr<ExpressionNode> buildExpressionTree(const std::vector<std::string>& tokens);
    std::vector<std::string> getWhereTokens(const std::vector<std::string>& tokens);

    // Обработчики конкретных SQL команд
    void handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm);
};

#endif // SQL_PARSER_H