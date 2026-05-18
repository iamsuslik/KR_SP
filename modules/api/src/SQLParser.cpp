#include "../include/SQLParser.h"
#include <map>
#include <stdexcept>
#include <stack>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <regex>

std::string SQLParser::toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

bool SQLParser::isValidCase(const std::string& token) {
    if (token.empty() || token[0] == '"') return true;
    bool hasUpper = false, hasLower = false;
    for (char c : token) {
        if (std::isupper(static_cast<unsigned char>(c))) hasUpper = true;
        if (std::islower(static_cast<unsigned char>(c))) hasLower = true;
    }
    return !(hasUpper && hasLower);
}

bool SQLParser::isValidIdentifier(const std::string& name) {
    std::regex pattern("^[a-zA-Z_][a-zA-Z0-9_]*$");
    return std::regex_match(name, pattern);
}

int SQLParser::getPrecedence(const std::string& op) {
    if (op == "OR") return 1;
    if (op == "AND") return 2;
    if (op == "=" || op == ">" || op == "<" || op == "!=" || op == ">=" || op == "<=" || op == "LIKE") return 3;
    return 0;
}

std::shared_ptr<ExpressionNode> SQLParser::buildExpressionTree(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return nullptr;

    std::stack<std::shared_ptr<ExpressionNode>> values;
    std::stack<std::string> ops;

    auto processOp = [&]() {
        if (ops.empty() || values.size() < 2) return;
        std::string op = ops.top(); ops.pop();
        auto right = values.top(); values.pop();
        auto left = values.top(); values.pop();

        auto node = std::make_shared<ExpressionNode>();
        if (op == "AND" || op == "OR") {
            node->is_op = true;
            node->op = op;
            node->left = left;
            node->right = right;
        } else {
            node->is_op = false;
            node->op = op;
            node->column = left->column; 
            node->val1 = right->column;
        }
        values.push(node);
    };

    for (const auto& token : tokens) {
        std::string upToken = toUpper(token);

        if (upToken == "(") {
            ops.push(upToken);
        } else if (upToken == ")") {
            while (!ops.empty() && ops.top() != "(") processOp();
            if (!ops.empty()) ops.pop();
        } else if (getPrecedence(upToken) > 0) {
            while (!ops.empty() && ops.top() != "(" && getPrecedence(ops.top()) >= getPrecedence(upToken)) {
                processOp();
            }
            ops.push(upToken);
        } else {
            auto leaf = std::make_shared<ExpressionNode>();
            leaf->is_op = false;
            leaf->column = (token.front() == '"') ? token.substr(1, token.size() - 2) : token;
            values.push(leaf);
        }
    }

    while (!ops.empty()) processOp();
    return values.empty() ? nullptr : values.top();
}

std::vector<std::string> SQLParser::tokenize(const std::string& query) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;

    for (size_t i = 0; i < query.length(); ++i) {
        char c = query[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            current += c;
            if (!inQuotes) { tokens.push_back(current); current = ""; }
        } else if (inQuotes) {
            current += c;
        } else if (isspace(static_cast<unsigned char>(c)) || c == ',' || c == '(' || c == ')' || c == ';') {
            if (!current.empty()) tokens.push_back(current);
            if (!isspace(static_cast<unsigned char>(c)) && c != ';') tokens.push_back(std::string(1, c));
            current = "";
        } else if (c == '=' || c == '<' || c == '>' || c == '!') {
            if (!current.empty()) tokens.push_back(current);
            std::string op(1, c);
            if (i + 1 < query.length() && query[i+1] == '=') { op += "="; i++; }
            tokens.push_back(op);
            current = "";
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}



void SQLParser::process(std::string query, HierarchyManager& hm) {
    auto tokens = tokenize(query);
    if (tokens.empty()) return;

    for (const auto& t : tokens) {
        if (t.front() != '"' && !isValidCase(t)) {
            std::cout << "[Error] Mixed case in word '" << t << "' is forbidden.\n";
            return;
        }
    }

    std::string cmd = toUpper(tokens[0]);
    if (cmd == "CREATE") {
        if (tokens.size() > 1 && toUpper(tokens[1]) == "DATABASE") handleCreateDatabase(tokens, hm);
        else if (tokens.size() > 1 && toUpper(tokens[1]) == "TABLE") handleCreateTable(tokens, hm);
    }
    else if (cmd == "USE") handleUse(tokens, hm);
    else if (cmd == "INSERT") handleInsert(tokens, hm);
    else if (cmd == "SELECT") handleSelect(tokens, hm);
    else if (cmd == "DELETE") handleDelete(tokens, hm);
    else if (cmd == "UPDATE") handleUpdate(tokens, hm);
    else if (cmd == "DROP") {
        if (tokens.size() > 2 && toUpper(tokens[1]) == "TABLE") {
            auto res = hm.resolveTablePath(tokens[2]);
            if (res.success && res.message == "EXIST") {
                std::cout << TableManager::dropTable(res.path).message << "\n";
            } else std::cout << "[Error] Table not found.\n";
        } else if (tokens.size() > 2 && toUpper(tokens[1]) == "DATABASE") {
            std::cout << hm.dropDatabase(tokens[2]).message << "\n";
        }
    }
    else std::cout << "[Error] Unknown command.\n";
}

std::vector<std::string> getWhereTokens(const std::vector<std::string>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (SQLParser::toUpper(tokens[i]) == "WHERE") {
            return std::vector<std::string>(tokens.begin() + i + 1, tokens.end());
        }
    }
    return {};
}

void SQLParser::handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 3) return;
    std::cout << hm.createDatabase(tokens[2]).message << "\n";
}

void SQLParser::handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 2) return;
    std::cout << hm.useDatabase(tokens[1]).message << "\n";
}

void SQLParser::handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 6) return;
    std::string tableName = tokens[2];
    auto res = hm.resolveTablePath(tableName);
    if (!res.success) { std::cout << res.message << "\n"; return; }
    if (res.message == "EXIST") { std::cout << "[Error] Table already exists.\n"; return; }

    std::vector<ColumnDef> cols;
    size_t i = 4;
    while (i < tokens.size() && tokens[i] != ")") {
        std::string colName = tokens[i++];
        std::string colTypeStr = toUpper(tokens[i++]);
        DataType type = (colTypeStr == "INT") ? DataType::INT : DataType::STR;
        
        ColumnDef cd(colName, type);
        while (i < tokens.size() && tokens[i] != "," && tokens[i] != ")") {
            std::string flag = toUpper(tokens[i++]);
            if (flag == "INDEXED") cd.is_indexed = true;
            else if (flag == "NOT_NULL") cd.is_not_null = true;
            else if (flag == "DEFAULT") {
                cd.has_default = true;
                cd.default_value = tokens[i++];
            }
        }
        cols.push_back(cd);
        if (i < tokens.size() && tokens[i] == ",") i++;
    }
    std::cout << TableManager::createTable(res.path, TableSchema(tableName, cols)).message << "\n";
}

void SQLParser::handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 5) return;
    std::string tableName = tokens[2];
    auto pathRes = hm.resolveTablePath(tableName);
    if (!pathRes.success || pathRes.message == "NEW") { std::cout << "[Error] Table not found.\n"; return; }

    TableHeader header;
    try { Pager p(pathRes.path); p.read_page(0, &header); }
    catch (...) { std::cout << "[Error] Schema read failed.\n"; return; }

    size_t valueTokenStart = 0;
    std::vector<std::string> targetColNames;

    if (tokens[3] == "(") {
        size_t i = 4;
        while (i < tokens.size() && tokens[i] != ")") { if (tokens[i] != ",") targetColNames.push_back(tokens[i]); i++; }
        for (size_t j = i; j < tokens.size(); ++j) { if (toUpper(tokens[j]) == "VALUE") { valueTokenStart = j + 2; break; } }
    } else {
        for (size_t j = 3; j < tokens.size(); ++j) { if (toUpper(tokens[j]) == "VALUE") { valueTokenStart = j + 2; break; } }
    }

    if (valueTokenStart == 0) { std::cout << "[Error] Syntax error: VALUE expected\n"; return; }

    std::vector<std::string> values;
    for (size_t vIdx = valueTokenStart; vIdx < tokens.size() && tokens[vIdx] != ")"; ++vIdx) {
        if (tokens[vIdx] != ",") values.push_back(tokens[vIdx]);
    }



    Row finalRow(header.column_count, Value());
    try {
        if (!targetColNames.empty()) {
            for (size_t i = 0; i < targetColNames.size() && i < values.size(); ++i) {
                int cIdx = -1;
                for (uint32_t c = 0; c < header.column_count; ++c) if (header.columns[c].name == targetColNames[i]) { cIdx = c; break; }
                if (cIdx != -1) {
                    if (header.columns[cIdx].type == 0) finalRow[cIdx] = Value(std::stoi(values[i]));
                    else { std::string s = values[i]; if (s.front() == '"') s = s.substr(1, s.size()-2); finalRow[cIdx] = Value(s); }
                }
            }
        } else {
            for (uint32_t i = 0; i < header.column_count; ++i) {
                if (header.columns[i].type == 0) finalRow[i] = Value(std::stoi(values[i]));
                else { std::string s = values[i]; if (s.front() == '"') s = s.substr(1, s.size()-2); finalRow[i] = Value(s); }
            }
        }
        std::cout << TableManager::insertRow(pathRes.path, finalRow).message << "\n";
    } catch (...) { std::cout << "[Error] Invalid value format in INSERT.\n"; }
}

void SQLParser::handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    std::string tableName;
    std::vector<std::string> selectedCols;
    std::vector<AggregateRequest> aggs;
    std::map<std::string, std::string> aliases;
    size_t fromIdx = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "FROM") { fromIdx = i; tableName = tokens[i+1]; break; }
    }
    if (tokens[1] != "*") {
        for (size_t i = 1; i < fromIdx; ++i) {
            std::string token = toUpper(tokens[i]);
            if (token == "SUM" || token == "COUNT" || token == "AVG") {
                AggregateType type = AggregateType::NONE;
                if (token == "SUM") type = AggregateType::SUM;
                else if (token == "COUNT") type = AggregateType::COUNT;
                else if (token == "AVG") type = AggregateType::AVG;
                if (i + 3 < fromIdx && tokens[i+1] == "(" && tokens[i+3] == ")") {
                    aggs.push_back({type, tokens[i+2]});
                    i += 3;
                    continue;
                }
            }

            if (tokens[i] == ",") continue;
            if (toUpper(tokens[i]) == "AS") {
                std::string alias = tokens[i+1];
                if (alias.front() == '"') alias = alias.substr(1, alias.size() - 2);
                if (!selectedCols.empty()) {
                    aliases[selectedCols.back()] = alias;
                }
                i++;
            } else {
                selectedCols.push_back(tokens[i]);
            }
        }
    }

    auto res = hm.resolveTablePath(tableName);
    if (res.success && res.message == "EXIST") {
        auto whereTokens = getWhereTokens(tokens);
        TableManager::executeSelect(res.path, buildExpressionTree(whereTokens).get(), selectedCols, aliases);
    } else std::cout << "[Error] Table not found.\n";

}

void SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 3) return;
    auto res = hm.resolveTablePath(tokens[2]);
    if (res.success && res.message == "EXIST") {
        auto whereTokens = getWhereTokens(tokens);
        std::cout << TableManager::executeDelete(res.path, buildExpressionTree(whereTokens).get()).message << "\n";
    } else std::cout << "[Error] Table not found.\n";
}

void SQLParser::handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 6) return;
    auto res = hm.resolveTablePath(tokens[1]);
    if (res.success && res.message == "EXIST") {
        auto whereTokens = getWhereTokens(tokens);
        std::cout << TableManager::executeUpdate(res.path, buildExpressionTree(whereTokens).get(), tokens[3], tokens[5]).message << "\n";
    } else std::cout << "[Error] Table not found.\n";
}
