#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <string>
#include <vector>
#include <regex>
#include <iostream>
#include <algorithm>

#include "../../shared/include/common.h"
#include "../../core/include/HierarchyManager.h"
#include "../../logic/include/TableManager.h"

class SQLParser {
public:
    void process(std::string query, HierarchyManager& hm);

private:
    std::vector<std::string> tokenize(const std::string& query);
    bool isValidCase(const std::string& token);
    bool isValidIdentifier(const std::string& name);
    std::string toUpper(std::string s);
    
    Condition parseWhere(const std::vector<std::string>& tokens);

    void handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm);
    void handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm);
};

#endif
