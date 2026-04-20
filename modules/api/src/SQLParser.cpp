#include "../include/SQLParser.h"

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
            std::string path; hm.prepareTablePath(tokens[2], path);
            std::cout << TableManager::dropTable(path).message << "\n";
        }
    }
    else std::cout << "[Error] Unknown command.\n";
}

void SQLParser::handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 3) return;
    if (isValidIdentifier(tokens[2])) std::cout << hm.createDatabase(tokens[2]).message << "\n";
    else std::cout << "[Error] Invalid database name.\n";
}

void SQLParser::handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 2) return;
    std::cout << hm.useDatabase(tokens[1]).message << "\n";
}

void SQLParser::handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    if (tokens.size() < 6) return;
    std::string tableName = tokens[2];
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
    std::string path;
    if (hm.prepareTablePath(tableName, path).success) {
        std::cout << TableManager::createTable(path, TableSchema(tableName, cols)).message << "\n";
    }
}

void SQLParser::handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    size_t valuePos = 0;
    for(size_t i=0; i<tokens.size(); ++i) if(toUpper(tokens[i]) == "VALUE") valuePos = i;
    if (valuePos == 0) return;

    std::string tableName = tokens[2];
    std::string path; 
    if (!hm.prepareTablePath(tableName, path).success) return;

    TableHeader header;
    try { Pager p(path); p.read_page(0, &header); } catch(...) { return; }

    Row row;
    size_t valStart = valuePos + 2;
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (valStart >= tokens.size()) break;
        std::string v = tokens[valStart];
        if (header.columns[i].type == 0) row.push_back(Value(std::stoi(v)));
        else {
            if (v.front() == '"') v = v.substr(1, v.size() - 2);
            row.push_back(Value(v));
        }
        valStart += 2;
    }
    std::cout << TableManager::insertRow(path, row).message << "\n";
}

void SQLParser::handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    std::string tableName;
    for(size_t i=0; i<tokens.size(); ++i) {
        if(toUpper(tokens[i]) == "FROM") {
            tableName = tokens[i+1];
            break;
        }
    }
    std::string path;
    if (hm.prepareTablePath(tableName, path).success) {
        TableManager::showAllRows(path, tableName);
    }
}

void SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    std::string tableName = tokens[2];
    Condition cond = parseWhere(tokens);
    std::string path;
    if (hm.prepareTablePath(tableName, path).success) {
        std::cout << TableManager::deleteRow(path, 1, 0).message << "\n"; 
    }
}

void SQLParser::handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm) {
    std::string tableName = tokens[1];
    std::cout << "[Info] UPDATE recognized for " << tableName << ". Pending implementation logic.\n";
}
