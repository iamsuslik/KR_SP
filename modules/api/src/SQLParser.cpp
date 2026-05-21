#include "SQLParser.h"
#include "common.h"
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

// bool SQLParser::isValidCase(const std::string& token) {
//     if (token.empty() || token[0] == '"') return true;
//     bool hasUpper = false, hasLower = false;
//     for (char c : token) {
//         if (std::isupper(static_cast<unsigned char>(c))) hasUpper = true;
//         if (std::islower(static_cast<unsigned char>(c))) hasLower = true;
//     }
//     return !(hasUpper && hasLower);
// }
Value SQLParser::parseLiteral(const std::string& token) {
    if (token.empty()) return Value();
    
    // Если это строка в кавычках (строковый литерал)
    if (token.front() == '"' && token.back() == '"') {
        return Value(token.substr(1, token.size() - 2));
    }
    
    // Попытка распарсить как число (целочисленный литерал)
    try {
        size_t pos;
        int val = std::stoi(token, &pos);
        // Если всё слово — это число
        if (pos == token.size()) return Value(val);
    } catch (...) {}

    // Если не число и не в кавычках, считаем это идентификатором или просто строкой
    return Value(token);
}

bool SQLParser::isValidIdentifier(const std::string& name) {
    std::regex pattern("^[a-zA-Z_][a-zA-Z0-9_]*$");
    return std::regex_match(name, pattern);
}

int SQLParser::getPrecedence(const std::string& op) {
    if (op == "OR") return 1;
    if (op == "AND") return 2;
    if (op == "=" || op == "==" || op == ">" || op == "<" || op == "!=" || op == ">=" || op == "<=" || op == "LIKE") return 3;
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

            node->val1_parsed = parseLiteral(right->column); 
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
    if (tokens[0].front() == '[') return;

    for (const auto& t : tokens) {
        // if (t.front() != '"' && !isValidCase(t)) {
        if (t.front() != '"') {
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

// Вспомогательный метод для выделения токенов после WHERE
std::vector<std::string> SQLParser::getWhereTokens(const std::vector<std::string>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "WHERE") {
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
    
    auto res = hm.resolveTablePath(tokens[2]);
    if (!res.success || res.message == "NEW") { std::cout << "[Error] Table not found.\n"; return; }

    TableHeader header;
    Pager(res.path).read_page(0, &header);

    std::vector<std::string> targetCols;
    size_t valStart = findValueStartIndex(tokens, targetCols);
    auto rawValues = collectValuesFromTokens(tokens, valStart);

    Row finalRow(header.column_count, Value());
    if (prepareAndValidateRow(finalRow, header, targetCols, rawValues)) {
        std::cout << TableManager::insertRow(res.path, finalRow).message << "\n";
    }
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
                if (!selectedCols.empty()) aliases[selectedCols.back()] = alias;
                i++;
            } else selectedCols.push_back(tokens[i]);
        }
    }

    auto res = hm.resolveTablePath(tableName);
    if (res.success && res.message == "EXIST") {
        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);
        TableManager::executeSelect(res.path, tree.get(), selectedCols, aliases, aggs);
    } else std::cout << "[Error] Table not found.\n";
}

void SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 3) return;
    auto res = hm.resolveTablePath(tokens[2]);
    if (res.success && res.message == "EXIST") {
        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);
        std::cout << TableManager::executeDelete(res.path, tree.get()).message << "\n";
    } else std::cout << "[Error] Table not found.\n";
}

void SQLParser::handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 6) return;
    auto res = hm.resolveTablePath(tokens[1]);
    if (res.success && res.message == "EXIST") {
        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);
        std::cout << TableManager::executeUpdate(res.path, tree.get(), tokens[3], tokens[5]).message << "\n";
    } else std::cout << "[Error] Table not found.\n";
}

size_t SQLParser::findValueStartIndex(const std::vector<std::string>& tokens, std::vector<std::string>& outColNames) {
    size_t i = 3;
    // Случай INSERT INTO table (c1, c2) ...
    if (i < tokens.size() && tokens[i] == "(") {
        i++; // Пропускаем (
        while (i < tokens.size() && tokens[i] != ")") {
            if (tokens[i] != ",") outColNames.push_back(tokens[i]);
            i++;
        }
        i++; // Пропускаем )
    }
    // Ищем слово VALUE
    while (i < tokens.size() && toUpper(tokens[i]) != "VALUE") i++;
    
    return (i + 2 < tokens.size()) ? i + 2 : 0; // Возвращаем индекс ПЕРВОГО значения после '('
}

std::vector<std::string> SQLParser::collectValuesFromTokens(const std::vector<std::string>& tokens, size_t startIdx) {
    std::vector<std::string> values;
    if (startIdx == 0) return values;
    for (size_t i = startIdx; i < tokens.size() && tokens[i] != ")"; ++i) {
        if (tokens[i] != ",") values.push_back(tokens[i]);
    }
    return values;
}

bool SQLParser::prepareAndValidateRow(Row& outRow, const TableHeader& header, 
                                     const std::vector<std::string>& targetCols, 
                                     const std::vector<std::string>& rawValues) {
    // 1. Проверка количества (если колонки не указаны явно)
    if (targetCols.empty() && rawValues.size() != header.column_count) {
        std::cout << "[Error] Column count mismatch. Expected " << header.column_count << ".\n";
        return false;
    }

    // 2. Наполнение Row (с использованием нашего parseLiteral)
    if (!targetCols.empty()) {
        for (size_t i = 0; i < targetCols.size() && i < rawValues.size(); ++i) {
            int cIdx = -1;
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (header.columns[c].name == targetCols[i]) { cIdx = c; break; }
            }
            if (cIdx != -1) outRow[cIdx] = parseLiteral(rawValues[i]);
        }
    } else {
        for (size_t i = 0; i < rawValues.size(); ++i) outRow[i] = parseLiteral(rawValues[i]);
    }

    // 3. Проверка NOT_NULL и применение DEFAULT
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (outRow[i].is_null) {
            if (header.columns[i].has_default) {
                outRow[i] = parseLiteral(header.columns[i].default_val);
            } else if (header.columns[i].is_not_null) {
                std::cout << "[Error] Column '" << header.columns[i].name << "' is NOT_NULL.\n";
                return false;
            }
        }
    }
    return true;
}