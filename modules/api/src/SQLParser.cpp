#include "SQLParser.h"
#include "ErrorUtils.h"
#include "DbException.h"
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
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { 
        return std::toupper(c); });
    return s;
}


Value SQLParser::parseLiteral(const std::string& token) const{
    if (token.empty()) return Value();

    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return Value(token.substr(1, token.size() - 2));
    }

    try {
        size_t pos;
        int val = std::stoi(token, &pos);
        if (pos == token.size()) return Value(val);
    } catch (...) {
        
    }

    return Value(token);
}

bool SQLParser::isValidIdentifier(const std::string& name) {
    if (name.empty() || name.size() > MAX_NAME_LEN) return false;
    
    static const std::regex pattern("^[a-zA-Z_][a-zA-Z0-9_]*$");
    return std::regex_match(name, pattern);
}

int SQLParser::getPrecedence(const std::string& op) {
    if (op == "OR")  return 1;
    if (op == "AND") return 2;
    if (op == "=" || op == "==" || op == ">" || op == "<" ||
        op == "!=" || op == ">=" || op == "<=" ||
        op == "LIKE" || op == "BETWEEN") return 3;
    return 0;
}

// Создание листа дерева (операнда)
std::shared_ptr<ExpressionNode> SQLParser::createLeaf(const std::string& token) {
    auto leaf = std::make_shared<ExpressionNode>();
    leaf->is_op = false;
    
    Value parsed = parseLiteral(token);
    leaf->column = (parsed.type == DataType::STR) ? parsed.str_val : std::to_string(parsed.int_val);
    return leaf;
}


void SQLParser::applyOperator(std::stack<std::shared_ptr<ExpressionNode>>& values,
                               std::stack<std::string>& ops) {
    if (ops.empty() || values.size() < 2) return;

    std::string op = ops.top(); ops.pop();
    auto right = values.top(); values.pop();
    auto left = values.top(); values.pop();

    auto node = std::make_shared<ExpressionNode>();
    node->op = op;

    if (op == "AND" || op == "OR") {
        node->is_op = true;
        node->left  = left;
        node->right = right;
    } else if (op == "BETWEEN") {
        node->is_op = false;
        node->column = left->column;
        node->op = "BETWEEN";
        node->val1_parsed = right->val1_parsed;
        node->val2_parsed = right->val2_parsed;
    } else {
        node->is_op = false;
        node->column = left->column;
        node->val1 = right->column;
        node->val1_parsed = parseLiteral(right->column);
    }
    values.push(node);
}

std::shared_ptr<ExpressionNode> SQLParser::buildExpressionTree(
        const std::vector<std::string>& tokens) {
    if (tokens.empty()) return nullptr;

    std::stack<std::shared_ptr<ExpressionNode>> values;
    std::stack<std::string> ops;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].empty()) continue;
        std::string upToken = toUpper(tokens[i]);

        if (upToken == "BETWEEN") {
            if (i + 3 < tokens.size() && toUpper(tokens[i + 2]) == "AND") {
                Value lo = parseLiteral(tokens[i + 1]);
                Value hi = parseLiteral(tokens[i + 3]);
                i += 3;

                if (values.empty()) continue;
                auto col_leaf = values.top(); values.pop();

                auto node = std::make_shared<ExpressionNode>();
                node->is_op = false;
                node->op = "BETWEEN";
                node->column = col_leaf->column;
                node->val1_parsed = lo;
                node->val2_parsed = hi;
                values.push(node);
            }
            continue;
        }

        if (upToken == "(") {
            ops.push(upToken);
        } else if (upToken == ")") {
            while (!ops.empty() && ops.top() != "(")
                applyOperator(values, ops);
            if (!ops.empty()) ops.pop();
        } else if (getPrecedence(upToken) > 0) {
            while (!ops.empty() && ops.top() != "(" &&
                   getPrecedence(ops.top()) >= getPrecedence(upToken))
                applyOperator(values, ops);
            ops.push(upToken);
        } else {
            values.push(createLeaf(tokens[i]));
        }
    }

    while (!ops.empty()) {
        if (ops.top() == "(") { ops.pop(); continue; }
        applyOperator(values, ops);
    }

    return values.empty() ? nullptr : values.top();
}

std::vector<std::string> SQLParser::tokenize(const std::string& query) const { 
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;

    auto pushCurrent = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };

    for (size_t i = 0; i < query.length(); ++i) {
        char c = query[i];
        
        if (c == '"') {
            inQuotes = !inQuotes;
            current += c;
            if (!inQuotes) pushCurrent(); 
            continue;
        }

        if (inQuotes) {
            current += c;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            pushCurrent();
        } 
        else if (c == ',' || c == '(' || c == ')' || c == ';') {
            pushCurrent();
            if (c != ';') {
                tokens.push_back(std::string(1, c));
            }
        } 
        else if (c == '=' || c == '<' || c == '>' || c == '!') {
            pushCurrent();
            std::string op(1, c);
            
            if (i + 1 < query.length() && query[i + 1] == '=') { 
                op += "="; 
                i++; 
            }
            tokens.push_back(op);
        } 
        else {
            current += c;
        }
    }
    
    pushCurrent(); 
    return tokens;
}

// для проверки регистра 
Result SQLParser::validateTokenCase(const std::vector<std::string>& tokens, OutputCallback callback) {
    for (const auto& t : tokens) {
        if (t.empty() || t.front() == '"') continue;

        bool hasUpper = false;
        bool hasLower = false;
        for (char c : t) {
            if (std::isupper(static_cast<unsigned char>(c))) hasUpper = true;
            if (std::islower(static_cast<unsigned char>(c))) hasLower = true;
        }

        if (hasUpper && hasLower) {
            std::string msg = "[Error] Mixed case in word '" + t + "' is forbidden.\n";
            callback(msg);
            return Result::Error(StatusCode::SYNTAX_ERROR, msg);
        }
    }
    return Result::Success();
}

// логика DROP
Result SQLParser::handleDrop(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 3) {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Incomplete DROP command");
    }

    std::string sub = toUpper(tokens[1]);
    Result final_res; 

    try {
        if (sub == "TABLE") {
            auto res = hm.resolveTablePath(tokens[2]);
            res.throw_if_error();  
            
            final_res = TableManager::dropTable(res.path);
        } 
        else if (sub == "DATABASE") {
            final_res = hm.dropDatabase(tokens[2]);
        } 
        else {
            return Result::Error(StatusCode::SYNTAX_ERROR, "Unknown DROP target: " + sub);
        }

        final_res.throw_if_error();
        
        callback("[Success] " + final_res.details + "\n");
        return final_res;
        
    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err_res) + "\n");
        return err_res;
    }
}


Result SQLParser::process(const std::string& query, HierarchyManager& hm, OutputCallback callback) {
    try {
        auto tokens = tokenize(query);
        if (tokens.empty()) return Result::Success();
        if (tokens[0].front() == '[') return Result::Success(); 

        validateTokenCase(tokens, callback).throw_if_error();

        std::string cmd = toUpper(tokens[0]);

        if (cmd == "CREATE") {
            if (tokens.size() < 2) return Result::Error(StatusCode::SYNTAX_ERROR, "Incomplete CREATE");
            std::string sub = toUpper(tokens[1]);
            if (sub == "DATABASE") return handleCreateDatabase(tokens, hm, callback);
            if (sub == "TABLE")    return handleCreateTable(tokens, hm, callback);
        }
        
        if (cmd == "USE")    return handleUse(tokens, hm, callback);
        if (cmd == "INSERT") return handleInsert(tokens, hm, callback);
        if (cmd == "SELECT") return handleSelect(tokens, hm, callback);
        if (cmd == "DELETE") return handleDelete(tokens, hm, callback);
        if (cmd == "UPDATE") return handleUpdate(tokens, hm, callback);
        if (cmd == "DROP")   return handleDrop(tokens, hm, callback);

        std::string err = "[Error] Unknown command: " + tokens[0] + "\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    }
}

std::vector<std::string> SQLParser::getWhereTokens(const std::vector<std::string>& tokens) const {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "WHERE") {
            return std::vector<std::string>(tokens.begin() + i + 1, tokens.end());
        }
    }
    return {};
}

Result SQLParser::handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 3) {
        std::string err = "[Error] Syntax error. Expected: CREATE DATABASE [database_name];\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    try {
        Result res = hm.createDatabase(tokens[2]);
        res.throw_if_error(); 

        callback("[Success] " + res.details + "\n");
        return res;

    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err_res) + "\n");
        return err_res;
    }
}

Result SQLParser::handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 2) {
        std::string err = "[Error] Syntax error. Expected: USE [database_name];\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    try {
        Result res = hm.useDatabase(tokens[1]);
        res.throw_if_error();

        callback("[Success] " + res.details + "\n");
        return res;

    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err_res) + "\n");
        return err_res;
    }
}

Result SQLParser::parseColumnDefinitions(const std::vector<std::string>& tokens, size_t& i, std::vector<ColumnDef>& outCols) {
    while (i < tokens.size() && tokens[i] != ")") {
        if (i + 1 >= tokens.size()) return Result::Error(StatusCode::SYNTAX_ERROR, "Unexpected end of column list");

        std::string colName = tokens[i++];
        std::string colTypeStr = toUpper(tokens[i++]);
        
        DataType type = (colTypeStr == "INT") ? DataType::INT : DataType::STR;
        ColumnDef cd(colName, type);

        while (i < tokens.size() && tokens[i] != "," && tokens[i] != ")") {
            std::string flag = toUpper(tokens[i++]);
            
            if (flag == "INDEXED") {
                cd.is_indexed = true;
            } else if (flag == "NOT_NULL") {
                cd.is_not_null = true;
            } else if (flag == "DEFAULT") {
                cd.has_default = true;
                if (i < tokens.size()) {
                    cd.default_value = tokens[i++];
                }
            }
        }
        
        outCols.push_back(cd);

        if (i < tokens.size() && tokens[i] == ",") {
            ++i;
        }
    }
    return Result::Success();
}

Result SQLParser::handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 6) {
        std::string err = "[Error] Syntax error. Expected: CREATE TABLE table_name (col1 type [flags], ...);\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    try {
        std::string tableName = tokens[2];
        Result res = hm.resolveTablePath(tableName);

        if (res.code == StatusCode::DATABASE_NOT_FOUND) {
            res.throw_if_error();
        }

        if (res.isOk()) {
            throw DbException(StatusCode::ALREADY_EXISTS, "Table '" + tableName + "' already exists.");
        }

        std::vector<ColumnDef> cols;
        size_t index = 4; // Сразу после '('
        parseColumnDefinitions(tokens, index, cols).throw_if_error();

        Result create_res = TableManager::createTable(res.path, TableSchema(tableName, cols));
        create_res.throw_if_error();
        
        callback("[Success] Table '" + tableName + "' created successfully.\n");
        return create_res;

    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + std::string(e.what()) + "\n");
        return err_res;
    }
}

Result SQLParser::handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 5) {
        std::string err = "[Error] Syntax error. Expected: INSERT INTO [table] (cols) VALUE (vals);\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    try {
        Result res = hm.resolveTablePath(tokens[2]);
        res.throw_if_error();

        TableHeader header;
        try {
            Pager(res.path).read_page(0, &header).throw_if_error();
        } catch (...) {
            throw DbException(StatusCode::IO_ERROR, "Failed to read table schema from disk.");
        }

        std::vector<std::string> targetCols;
        size_t valStart = findValueStartIndex(tokens, targetCols);
        auto rawValues = collectValuesFromTokens(tokens, valStart);

        Row finalRow(header.column_count, Value());

        if (!prepareAndValidateRow(finalRow, header, targetCols, rawValues, callback)) {
            return Result::Error(StatusCode::INVALID_VALUE, "Row validation failed");
        }
            
        Result insert_res = TableManager::insertRow(res.path, finalRow);
        insert_res.throw_if_error();

        callback("[Success] " + insert_res.details + "\n");
        return insert_res;

    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err) + "\n");
        return err_res;
    }
}

size_t SQLParser::findKeyword(const std::vector<std::string>& tokens, const std::string& keyword) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == keyword) return i;
    }
    return std::string::npos;
}

// логика парсинга колонок, агрегатов и алиасов
Result SQLParser::parseProjection(const std::vector<std::string>& tokens, size_t fromIdx, 
                                 std::vector<std::string>& selectedCols, 
                                 std::vector<AggregateRequest>& aggs, 
                                 std::map<std::string, std::string>& aliases) {
    if (fromIdx == 2 && tokens[1] == "*") return Result::Success();

    for (size_t i = 1; i < fromIdx; ++i) {
        std::string token = toUpper(tokens[i]);
        if (token == ",") continue;

        // Логика агрегатов (SUM, COUNT, AVG)
        if (token == "SUM" || token == "COUNT" || token == "AVG") {
            if (i + 3 < fromIdx && tokens[i+1] == "(" && tokens[i+3] == ")") {
                AggregateType type = (token == "SUM") ? AggregateType::SUM : 
                                    (token == "COUNT") ? AggregateType::COUNT : AggregateType::AVG;
                aggs.push_back({type, tokens[i+2]});
                i += 3; 
                continue;
            }
        }

        // Логика алиасов (AS)
        if (toUpper(tokens[i]) == "AS") {
            if (i + 1 < fromIdx) {
                std::string alias = tokens[i+1];
                if (alias.front() == '"') alias = alias.substr(1, alias.size() - 2);
                if (!selectedCols.empty()) aliases[selectedCols.back()] = alias;
                ++i;
            }
        } else {
            selectedCols.push_back(tokens[i]);
        }
    }
    return Result::Success();
}

Result SQLParser::handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {

    size_t fromIdx = findKeyword(tokens, "FROM");
    if (fromIdx == std::string::npos || fromIdx + 1 >= tokens.size()) {
        std::string err = "[Error] Syntax error. Expected FROM [table_name]\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    std::string tableName = tokens[fromIdx + 1];
    std::vector<std::string> selectedCols;
    std::vector<AggregateRequest> aggs;
    std::map<std::string, std::string> aliases;

    try {
        parseProjection(tokens, fromIdx, selectedCols, aggs, aliases).throw_if_error();

        auto res = hm.resolveTablePath(tableName);
        res.throw_if_error();

        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);
    
        Result select_res = TableManager::executeSelect(res.path, tree.get(), selectedCols, aliases, aggs, callback);
        select_res.throw_if_error();
        
        return select_res;

    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err_res) + "\n");
        return err_res;
    }
}

Result SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {

    if (tokens.size() < 3 || toUpper(tokens[1]) != "FROM") {
        std::string err = "[Error] Syntax error. Expected: DELETE FROM [table_name] <WHERE condition>;\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    try {
        auto res = hm.resolveTablePath(tokens[2]);
        res.throw_if_error();

        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);

        Result del_res = TableManager::executeDelete(res.path, tree.get());
        del_res.throw_if_error();

        callback("[Success] " + del_res.details + "\n");
        return del_res;

    } catch (const DbException& e) {
        Result err_res = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err_res) + "\n");
        return err_res;
    }
}

Result SQLParser::handleUpdate(const std::vector<std::string>& tokens,
                                HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 6 || toUpper(tokens[2]) != "SET") {
        std::string err = "[Error] Expected: UPDATE [table] SET col=val,... <WHERE cond>;\n";
        callback(err); 
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }
    try {
        auto res = hm.resolveTablePath(tokens[1]);
        res.throw_if_error();

        std::vector<std::pair<std::string,std::string>> assignments;
        size_t i = 3;
        while (i < tokens.size() && toUpper(tokens[i]) != "WHERE") {
            if (tokens[i] == ",") { 
                ++i; 
                continue; 
            }
            if (i + 2 < tokens.size() && tokens[i + 1] == "=") {
                assignments.emplace_back(tokens[i], tokens[i + 2]);
                i += 3;
            } else {
                ++i;
            }
        }

        if (assignments.empty()) {
            std::string err = "[Error] No assignments in SET clause.\n";
            callback(err); 
            return Result::Error(StatusCode::SYNTAX_ERROR, err);
        }

        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);

        Result upd = TableManager::executeUpdate(res.path, tree.get(), assignments);
        upd.throw_if_error();
        callback("[Success] " + upd.details + "\n");
        return upd;
    } catch (const DbException& e) {
        Result err = Result::Error(e.code(), e.what());
        callback("[Error] " + ErrorUtils::formatMessage(err) + "\n");
        return err;
    }
}

size_t SQLParser::findValueStartIndex(const std::vector<std::string>& tokens, std::vector<std::string>& outColNames) const {
    size_t i = 3;

    if (i < tokens.size() && tokens[i] == "(") {
        ++i;
        while (i < tokens.size() && tokens[i] != ")") {
            if (tokens[i] != ",") {
                outColNames.push_back(tokens[i]);
            }
            ++i;
        }
        if (i < tokens.size()){ 
            ++i;
        }
    }

    while (i < tokens.size() && toUpper(tokens[i]) != "VALUE") {
        ++i;
    }
    
    if (i + 1 < tokens.size() && tokens[i+1] == "(") {
        return i + 2; 
    }

    return std::string::npos; 
}

std::vector<std::string> SQLParser::collectValuesFromTokens(const std::vector<std::string>& tokens, size_t startIdx) const {
    std::vector<std::string> values;
    
    if (startIdx == std::string::npos || startIdx >= tokens.size()) {
        return values;
    }

    for (size_t i = startIdx; i < tokens.size() && tokens[i] != ")"; ++i) {
        if (tokens[i] != ",") {
            values.push_back(tokens[i]);
        }
    }
    return values;
}

int SQLParser::findColumnIndex(const TableHeader& header, const std::string& colName) const {
    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (std::string(header.columns[c].name) == colName) return (int)c;
    }
    return -1;
}

bool SQLParser::applyConstraints(Row& row, const TableHeader& header, OutputCallback callback) const {
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (row[i].is_null) {
            if (header.columns[i].has_default) {
                row[i] = parseLiteral(header.columns[i].default_val);
            } 
            else if (header.columns[i].is_not_null) {
                callback("[Error] Constraint Violation: Column '" + std::string(header.columns[i].name) + "' is NOT_NULL.\n");
                return false;
            }
        }
    }
    return true;
}

bool SQLParser::prepareAndValidateRow(Row& outRow, const TableHeader& header, 
                                     const std::vector<std::string>& targetCols, 
                                     const std::vector<std::string>& rawValues,
                                     OutputCallback callback) {

    if (targetCols.empty() && rawValues.size() != header.column_count) {
        callback("[Error] Column count mismatch. Expected " + std::to_string(header.column_count) + " values.\n");
        return false;
    }

    if (!targetCols.empty()) {
        for (size_t i = 0; i < targetCols.size() && i < rawValues.size(); ++i) {
            int cIdx = findColumnIndex(header, targetCols[i]);
            if (cIdx != -1) {
                outRow[cIdx] = parseLiteral(rawValues[i]);
            } else {
                callback("[Error] Column '" + targetCols[i] + "' not found.\n");
                return false;
            }
        }
    } else {
        for (size_t i = 0; i < rawValues.size() && i < header.column_count; ++i) {
            outRow[i] = parseLiteral(rawValues[i]);
        }
    }

    return applyConstraints(outRow, header, callback);
}
