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

Token SQLParser::peek() const {
    return token_stream[pos];
}

Token SQLParser::consume() {
    return token_stream[pos++];
}

bool SQLParser::match(TokenType type, const std::string& val) {
    if (pos >= token_stream.size()) return false;
    if (token_stream[pos].type == type) {
        if (val.empty() || toUpper(token_stream[pos].value) == toUpper(val)) {
            pos++;
            return true;
        }
    }
    return false;
}

std::unique_ptr<ASTNode> SQLParser::parseExpression() {
    auto node = parseAnd();
    while (match(TokenType::KEYWORD, "OR")) {
        auto right = parseAnd();
        node = std::make_unique<LogicalNode>("OR", std::move(node), std::move(right));
    }
    return node;
}

std::unique_ptr<ASTNode> SQLParser::parseAnd() {
    auto node = parsePrimary();
    while (match(TokenType::KEYWORD, "AND")) {
        auto right = parsePrimary();
        node = std::make_unique<LogicalNode>("AND", std::move(node), std::move(right));
    }
    return node;
}

std::unique_ptr<ASTNode> SQLParser::parsePrimary() {
    if (match(TokenType::LPAREN)) {
        auto node = parseExpression();
        match(TokenType::RPAREN);
        return node;
    }

    Token col = consume(); 

    if (match(TokenType::KEYWORD, "BETWEEN")) {
        Value low = parseLiteral(consume().value);
        match(TokenType::KEYWORD, "AND");
        Value high = parseLiteral(consume().value);
        return std::make_unique<BetweenNode>(col.value, low, high);
    }

    Token op = consume();
    Value val = parseLiteral(consume().value);
    return std::make_unique<ComparisonNode>(col.value, op.value, val);
}

std::vector<std::string> SQLParser::getLegacyTokens(const std::vector<Token>& tokens) {
    std::vector<std::string> legacy;
    for (const auto& t : tokens) {
        if (t.type != TokenType::END_OF_FILE) {
            legacy.push_back(t.value);
        }
    }
    return legacy;
}

TokenType SQLParser::getTokenType(const std::string& value) const {
    std::string up = toUpper(value);
    static const std::unordered_set<std::string> keywords = {
        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUE", "VALUES",
        "CREATE", "TABLE", "DATABASE", "DROP", "USE", "UPDATE", "SET", 
        "DELETE", "AND", "OR", "BETWEEN", "LIKE", "INT", "STR", 
        "NOT_NULL", "INDEXED", "AS", "SUM", "COUNT", "AVG", "DEFAULT"
    };

    if (keywords.count(up)) return TokenType::KEYWORD;
    if (value.front() == '"') return TokenType::LITERAL;
    if (std::isdigit(static_cast<unsigned char>(value[0]))) return TokenType::LITERAL;
    
    return TokenType::IDENTIFIER;
}

std::vector<Token> SQLParser::lex(const std::string& query) {
    std::vector<Token> result;
    std::string current;
    bool inQuotes = false;

    auto pushCurrent = [&]() {
        if (current.empty()) return;
        result.push_back({getTokenType(current), current});
        current.clear();
    };

    for (size_t i = 0; i < query.length(); ++i) {
        char c = query[i];

        if (c == '"') {
            if (inQuotes) {
                current += c;
                pushCurrent();
                inQuotes = false;
            } else {
                pushCurrent();
                inQuotes = true;
                current += c;
            }
            continue;
        }
        if (inQuotes) { current += c; continue; }

        if (std::isspace(static_cast<unsigned char>(c))) {
            pushCurrent();
            continue;
        }

        if (c == ',' || c == '(' || c == ')' || c == ';' || c == '*') {
            pushCurrent();
            TokenType type;
            if (c == ',') type = TokenType::COMMA;
            else if (c == '(') type = TokenType::LPAREN;
            else if (c == ')') type = TokenType::RPAREN;
            else if (c == ';') type = TokenType::SEMICOLON;
            else type = TokenType::STAR;
            result.push_back({type, std::string(1, c)});
            continue;
        }

        if (c == '=' || c == '<' || c == '>' || c == '!') {
            pushCurrent();
            std::string op(1, c);
            if (i + 1 < query.length() && query[i + 1] == '=') {
                op += "=";
                i++;
            }
            result.push_back({TokenType::OPERATOR, op});
            continue;
        }

        current += c;
    }

    pushCurrent();
    result.push_back({TokenType::END_OF_FILE, ""});
    return result;
}

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
        this->pos = 0;
        this->token_stream = lex(query);

        if (token_stream.empty() || token_stream[0].type == TokenType::END_OF_FILE) {
            return Result::Success();
        }

        std::vector<std::string> legacy_tokens = getLegacyTokens(token_stream);

        if (legacy_tokens.empty() || legacy_tokens[0].front() == '[') return Result::Success(); 

        validateTokenCase(legacy_tokens, callback).throw_if_error();

        std::string cmd = toUpper(legacy_tokens[0]);

        if (cmd == "CREATE") {
            if (legacy_tokens.size() < 2) return Result::Error(StatusCode::SYNTAX_ERROR, "Incomplete CREATE");
            std::string sub = toUpper(legacy_tokens[1]);
            if (sub == "DATABASE") return handleCreateDatabase(legacy_tokens, hm, callback);
            if (sub == "TABLE")    return handleCreateTable(legacy_tokens, hm, callback);
        }
        
        if (cmd == "USE")    return handleUse(legacy_tokens, hm, callback);
        if (cmd == "INSERT") return handleInsert(legacy_tokens, hm, callback);
        if (cmd == "SELECT") return handleSelect(legacy_tokens, hm, callback);
        if (cmd == "DELETE") return handleDelete(legacy_tokens, hm, callback);
        if (cmd == "UPDATE") return handleUpdate(legacy_tokens, hm, callback);
        if (cmd == "DROP")   return handleDrop(legacy_tokens, hm, callback);

        std::string err = "[Error] Unknown command: " + legacy_tokens[0] + "\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
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
        size_t index = 4;
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

Result SQLParser::parseProjection(const std::vector<std::string>& tokens, size_t fromIdx, 
                                 std::vector<std::string>& selectedCols, 
                                 std::vector<AggregateRequest>& aggs, 
                                 std::map<std::string, std::string>& aliases) {
    if (fromIdx == 2 && tokens[1] == "*") return Result::Success();

    for (size_t i = 1; i < fromIdx; ++i) {
        std::string token = toUpper(tokens[i]);
        if (token == ",") continue;

        if (token == "SUM" || token == "COUNT" || token == "AVG") {
            if (i + 3 < fromIdx && tokens[i+1] == "(" && tokens[i+3] == ")") {
                AggregateType type = (token == "SUM") ? AggregateType::SUM : 
                                    (token == "COUNT") ? AggregateType::COUNT : AggregateType::AVG;
                aggs.push_back({type, tokens[i+2]});
                i += 3; 
                continue;
            }
        }

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

Result SQLParser::handleSelect(const std::vector<std::string>& legacy_tokens, HierarchyManager& hm, OutputCallback callback) {
    size_t fromIdx = findKeyword(legacy_tokens, "FROM");
    if (fromIdx == std::string::npos || fromIdx + 1 >= legacy_tokens.size()) {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Expected: SELECT ... FROM [table_name]");
    }

    std::string tableName = legacy_tokens[fromIdx + 1];
    std::vector<std::string> selectedCols;
    std::vector<AggregateRequest> aggs;
    std::map<std::string, std::string> aliases;

    try {
        parseProjection(legacy_tokens, fromIdx, selectedCols, aggs, aliases).throw_if_error();

        auto res = hm.resolveTablePath(tableName);
        res.throw_if_error();

        std::unique_ptr<ASTNode> root_node = nullptr;
        for (size_t i = 0; i < token_stream.size(); ++i) {
            if (token_stream[i].type == TokenType::KEYWORD && toUpper(token_stream[i].value) == "WHERE") {
                this->pos = i + 1;
                root_node = parseExpression();
                break;
            }
        }

        return TableManager::executeSelect(res.path, root_node.get(), selectedCols, aliases, aggs, callback);

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    }
}

Result SQLParser::handleDelete(const std::vector<std::string>& legacy_tokens, HierarchyManager& hm, OutputCallback callback) {
    if (legacy_tokens.size() < 3 || toUpper(legacy_tokens[1]) != "FROM") {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Expected: DELETE FROM [table_name] <WHERE...>");
    }

    try {
        auto res = hm.resolveTablePath(legacy_tokens[2]);
        res.throw_if_error();

        std::unique_ptr<ASTNode> root_node = nullptr;
        for (size_t i = 0; i < token_stream.size(); ++i) {
            if (token_stream[i].type == TokenType::KEYWORD && toUpper(token_stream[i].value) == "WHERE") {
                this->pos = i + 1;
                root_node = parseExpression();
                break;
            }
        }

        Result del_res = TableManager::executeDelete(res.path, root_node.get());
        del_res.throw_if_error();

        callback("[Success] " + del_res.details + "\n");
        return del_res;

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    }
}

Result SQLParser::handleUpdate(const std::vector<std::string>& legacy_tokens, HierarchyManager& hm, OutputCallback callback) {
    if (legacy_tokens.size() < 6 || toUpper(legacy_tokens[2]) != "SET") {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Expected: UPDATE [table] SET col=val <WHERE...>");
    }

    try {
        auto res = hm.resolveTablePath(legacy_tokens[1]);
        res.throw_if_error();

        std::vector<std::pair<std::string, Value>> assignments;
        size_t i = 3; 
        
        while (i < legacy_tokens.size() && toUpper(legacy_tokens[i]) != "WHERE") {
            if (legacy_tokens[i] == ",") { i++; continue; }
            
            if (i + 2 < legacy_tokens.size() && legacy_tokens[i+1] == "=") {
                assignments.emplace_back(legacy_tokens[i], parseLiteral(legacy_tokens[i+2]));
                i += 3;
            } else {
                break; 
            }
        }

        if (assignments.empty()) return Result::Error(StatusCode::SYNTAX_ERROR, "No assignments found");

        std::unique_ptr<ASTNode> root_node = nullptr;
        for (size_t k = 0; k < token_stream.size(); ++k) {
            if (token_stream[k].type == TokenType::KEYWORD && toUpper(token_stream[k].value) == "WHERE") {
                this->pos = k + 1;
                root_node = parseExpression();
                break;
            }
        }

        Result upd_res = TableManager::executeUpdate(res.path, root_node.get(), assignments);
        upd_res.throw_if_error();

        callback("[Success] " + upd_res.details + "\n");
        return upd_res;

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
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
