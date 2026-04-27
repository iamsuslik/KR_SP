#include "../include/SQLParser.h"
#include <map>

std::string SQLParser::toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

bool SQLParser::isValidCase(const std::string& token) {
    if (token.empty() || token[0] == '"') return true;
    bool hasUpper = false, hasLower = false;
    for (char c : token) {
        if (std::isupper(c)) hasUpper = true;
        if (std::islower(c)) hasLower = true;
    }
    return !(hasUpper && hasLower);
}

bool SQLParser::isValidIdentifier(const std::string& name) {
    std::regex pattern("^[a-zA-Z_][a-zA-Z0-9_]*$");
    return std::regex_match(name, pattern);
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
        } else if (isspace(c) || c == ',' || c == '(' || c == ')' || c == ';') {
            if (!current.empty()) tokens.push_back(current);
            if (!isspace(c) && c != ';') tokens.push_back(std::string(1, c));
            current = "";
        } else if (c == '=' || c == '<' || c == '>' || c == '!') {
            if (!current.empty()) tokens.push_back(current);
            std::string op(1, c);
            if (i + 1 < query.length() && query[i+1] == '=') {
                op += "="; i++;
            }
            tokens.push_back(op);
            current = "";
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}


Condition SQLParser::parseWhere(const std::vector<std::string>& tokens) {
    Condition c;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "WHERE") {
            if (i + 3 >= tokens.size()) break;
            c.active = true;
            c.column = tokens[i+1];
            c.op = tokens[i+2];
            c.val1 = tokens[i+3];
            if (toUpper(c.op) == "BETWEEN" && i + 5 < tokens.size()) {
                c.val2 = tokens[i+5];
            }
            break;
        }
    }
    return c;
}

void SQLParser::process(std::string query, HierarchyManager& hm) {
    auto tokens = tokenize(query);
    if (tokens.empty()) return;

    for (const auto& t : tokens) {
        if (!isValidCase(t)) {
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
        }
    }
    else std::cout << "[Error] Unknown command.\n";
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
    if (res.message == "EXIST") {
        std::cout << "[Error] Table '" << tableName << "' already exists.\n";
        return;
    }

    std::vector<ColumnDef> cols;
    size_t i = 4;
    while (i < tokens.size() && tokens[i] != ")") {
        std::string colName = tokens[i++];
        std::string colTypeStr = toUpper(tokens[i++]);
        DataType type = (colTypeStr == "INT") ? DataType::INT : DataType::STR;
        bool isIdx = false, isNN = false;
        while (i < tokens.size() && tokens[i] != "," && tokens[i] != ")") {
            std::string flag = toUpper(tokens[i++]);
            if (flag == "INDEXED") isIdx = true;
            else if (flag == "NOT_NULL") isNN = true;
        }
        cols.push_back(ColumnDef(colName, type, isIdx, isNN));
        if (i < tokens.size() && tokens[i] == ",") i++;
    }
    std::cout << TableManager::createTable(res.path, TableSchema(tableName, cols)).message << "\n";
}

void SQLParser::handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 5) return;
    std::string tableName = tokens[2];
    
    auto pathRes = hm.resolveTablePath(tableName);
    if (!pathRes.success || pathRes.message == "NEW") {
        std::cout << "[Error] Table not found.\n";
        return;
    }

    TableHeader header;
    try {
        Pager p(pathRes.path);
        p.read_page(0, &header);
    } catch (...) {
        std::cout << "[Error] Failed to read table schema.\n";
        return;
    }

    size_t valueTokenStart = 0;
    std::vector<std::string> targetColNames;

    if (tokens[3] == "(") {
        size_t i = 4;
        while (i < tokens.size() && tokens[i] != ")") {
            if (tokens[i] != ",") targetColNames.push_back(tokens[i]);
            i++;
        }
        for (size_t j = i; j < tokens.size(); ++j) {
            if (toUpper(tokens[j]) == "VALUE") { valueTokenStart = j + 2; break; }
        }
    } else {
        for (size_t j = 3; j < tokens.size(); ++j) {
            if (toUpper(tokens[j]) == "VALUE") { valueTokenStart = j + 2; break; }
        }
    }

    if (valueTokenStart == 0) { std::cout << "[Error] Syntax error: missing VALUE\n"; return; }

    std::vector<std::string> values;
    for (size_t vIdx = valueTokenStart; vIdx < tokens.size() && tokens[vIdx] != ")"; ++vIdx) {
        if (tokens[vIdx] != ",") values.push_back(tokens[vIdx]);
    }

    Row finalRow(header.column_count, Value()); 

    if (!targetColNames.empty()) {
        for (size_t i = 0; i < targetColNames.size() && i < values.size(); ++i) {
            int colIdx = -1;
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == targetColNames[i]) { colIdx = c; break; }
            }
            if (colIdx != -1) {
                if (header.columns[colIdx].type == 0) finalRow[colIdx] = Value(std::stoi(values[i]));
                else {
                    std::string s = values[i];
                    if (s.front() == '"') s = s.substr(1, s.size()-2);
                    finalRow[colIdx] = Value(s);
                }
            }
        }
    } else {
        for (uint32_t i = 0; i < header.column_count && i < values.size(); ++i) {
            if (header.columns[i].type == 0) finalRow[i] = Value(std::stoi(values[i]));
            else {
                std::string s = values[i];
                if (s.front() == '"') s = s.substr(1, s.size()-2);
                finalRow[i] = Value(s);
            }
        }
    }

    std::cout << TableManager::insertRow(pathRes.path, finalRow).message << "\n";
}

void SQLParser::handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    std::string tableName;
    std::map<std::string, std::string> aliases;
    size_t fromIdx = 0;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "FROM") { fromIdx = i; tableName = tokens[i + 1]; break; }
    }

    for (size_t i = 1; i < fromIdx; ++i) {
        if (toUpper(tokens[i]) == "AS") {
            std::string alias = tokens[i + 1];
            if (alias.front() == '"') alias = alias.substr(1, alias.size() - 2);
            aliases[tokens[i - 1]] = alias;
        }
    }

    auto res = hm.resolveTablePath(tableName);
    if (res.success && res.message == "EXIST") {
        TableManager::executeSelect(res.path, parseWhere(tokens), aliases);
    } else std::cout << "[Error] Table not found.\n";
}

void SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 3) return;
    auto res = hm.resolveTablePath(tokens[2]);
    if (res.success && res.message == "EXIST") {
        std::cout << TableManager::executeDelete(res.path, parseWhere(tokens)).message << "\n";
    } else std::cout << "[Error] Table not found.\n";
}

void SQLParser::handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 6) return;
    auto res = hm.resolveTablePath(tokens[1]);
    if (res.success && res.message == "EXIST") {
        std::cout << TableManager::executeUpdate(res.path, parseWhere(tokens), tokens[3], tokens[5]).message << "\n";
    } else std::cout << "[Error] Table not found.\n";
}