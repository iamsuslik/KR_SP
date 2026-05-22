#include "SQLParser.h"
#include "ErrorUtils.h"
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

            // Используем парсер литералов для корректного преобразования типов
            node->val1_parsed = parseLiteral(right->column); 
        }
        values.push(node);
    };

    for (const auto& token : tokens) {
        if (token.empty()) continue;

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
            // ЛИСТ ДЕРЕВА (Операнд)
            auto leaf = std::make_shared<ExpressionNode>();
            leaf->is_op = false;
            
            Value parsed = parseLiteral(token);
            leaf->column = (parsed.type == DataType::STR) ? parsed.str_val : std::to_string(parsed.int_val);
            
            values.push(leaf);
        }
    }

    while (!ops.empty()) processOp();
    return values.empty() ? nullptr : values.top();
}

std::vector<std::string> SQLParser::tokenize(const std::string& query) const { 
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;

    for (size_t i = 0; i < query.length(); ++i) {
        char c = query[i];
        
        if (c == '"') {
            inQuotes = !inQuotes;
            current += c;
            // Если кавычка закрылась, сразу пушим токен
            if (!inQuotes) { 
                tokens.push_back(current); 
                current = ""; 
            }
        } else if (inQuotes) {
            current += c;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '(' || c == ')' || c == ';') {
            if (!current.empty()) tokens.push_back(current);
            // Пушим разделитель как отдельный токен (кроме пробелов и точки с запятой)
            if (!std::isspace(static_cast<unsigned char>(c)) && c != ';') {
                tokens.push_back(std::string(1, c));
            }
            current = "";
        } else if (c == '=' || c == '<' || c == '>' || c == '!') {
            if (!current.empty()) tokens.push_back(current);
            std::string op(1, c);
            // Проверка на составные операторы: <=, >=, !=, ==
            if (i + 1 < query.length() && query[i+1] == '=') { 
                op += "="; 
                i++; 
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

void SQLParser::process(const std::string& query, HierarchyManager& hm, OutputCallback callback) {
    // Вызываем токенизатор
    auto tokens = tokenize(query);
    if (tokens.empty()) return;
    
    // Проверка на мета-комментарии (если есть)
    if (tokens[0].front() == '[') return;

    // 1. Валидация регистра (Требование ТЗ)
    for (const auto& t : tokens) {
        if (t.front() != '"') { 
            bool hasUpper = false;
            bool hasLower = false;
            for (char c : t) {
                if (std::isupper(static_cast<unsigned char>(c))) hasUpper = true;
                if (std::islower(static_cast<unsigned char>(c))) hasLower = true;
            }
            if (hasUpper && hasLower) {
                callback("[Error] Mixed case in word '" + t + "' is forbidden by system rules.\n");
                return;
            }
        }
    }

    std::string cmd = toUpper(tokens[0]);
    
    // 2. Диспетчеризация команд с ПЕРЕДАЧЕЙ CALLBACK
    if (cmd == "CREATE") {
        if (tokens.size() > 1) {
            std::string sub = toUpper(tokens[1]);
            if (sub == "DATABASE") handleCreateDatabase(tokens, hm, callback);
            else if (sub == "TABLE") handleCreateTable(tokens, hm, callback);
        }
    }
    else if (cmd == "USE")    handleUse(tokens, hm, callback);
    else if (cmd == "INSERT") handleInsert(tokens, hm, callback);
    else if (cmd == "SELECT") handleSelect(tokens, hm, callback);
    else if (cmd == "DELETE") handleDelete(tokens, hm, callback);
    else if (cmd == "UPDATE") handleUpdate(tokens, hm, callback);
    else if (cmd == "DROP") {
        if (tokens.size() > 2) {
            std::string sub = toUpper(tokens[1]);
            if (sub == "TABLE") {
                auto res = hm.resolveTablePath(tokens[2]);
                if (res.isOk()) {
                    Result drop_res = TableManager::dropTable(res.path);
                    if (drop_res.isOk()) callback(drop_res.details + "\n");
                    else callback("[Error] " + ErrorUtils::formatMessage(drop_res) + "\n");
                } else {
                    callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
                }
            } 
            else if (sub == "DATABASE") {
                Result drop_res = hm.dropDatabase(tokens[2]);
                if (drop_res.isOk()) callback(drop_res.details + "\n");
                else callback("[Error] " + ErrorUtils::formatMessage(drop_res) + "\n");
            }
        }
    }
    else {
        callback("[Error] Unknown command: " + tokens[0] + "\n");
    }
}

// Вспомогательный метод для выделения токенов после WHERE
std::vector<std::string> SQLParser::getWhereTokens(const std::vector<std::string>& tokens) const{
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "WHERE") {
            return std::vector<std::string>(tokens.begin() + i + 1, tokens.end());
        }
    }
    return {};
}

void SQLParser::handleCreateDatabase(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 3) {
        callback("[Error] Syntax error. Expected: CREATE DATABASE [database_name];\n");
        return;
    }

    Result res = hm.createDatabase(tokens[2]);
    
    if (res.isOk()) {
        callback("[Success] " + res.details + "\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }
}

void SQLParser::handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 2) {
        callback("[Error] Syntax error. Expected: USE [database_name];\n");
        return;
    }

    Result res = hm.useDatabase(tokens[1]);
    
    if (res.isOk()) {
        callback("[Success] " + res.details + "\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }
}

void SQLParser::handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 6) {
        callback("[Error] Syntax error. Expected: CREATE TABLE table_name (col1 type [flags], ...);\n");
        return;
    }

    std::string tableName = tokens[2];

    Result res = hm.resolveTablePath(tableName);

    if (res.code == StatusCode::DATABASE_NOT_FOUND) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return;
    }

    if (res.isOk()) {
        callback("[Error] Table '" + tableName + "' already exists.\n");
        return; 
    }

    // 2. Парсинг схемы таблицы из токенов
    std::vector<ColumnDef> cols;
    size_t i = 4; // Пропускаем 'CREATE', 'TABLE', 'name', '('

    try {
        while (i < tokens.size() && tokens[i] != ")") {
            std::string colName = tokens[i++];
            std::string colTypeStr = toUpper(tokens[i++]);
            
            DataType type = (colTypeStr == "INT") ? DataType::INT : DataType::STR;
            ColumnDef cd(colName, type);

            // Обработка модификаторов (INDEXED, NOT_NULL, DEFAULT)
            while (i < tokens.size() && tokens[i] != "," && tokens[i] != ")") {
                std::string flag = toUpper(tokens[i++]);
                
                if (flag == "INDEXED") {
                    cd.is_indexed = true;
                } else if (flag == "NOT_NULL") {
                    cd.is_not_null = true;
                } else if (flag == "DEFAULT") {
                    // Реализация Задания №10
                    cd.has_default = true;
                    if (i < tokens.size()) {
                        cd.default_value = tokens[i++];
                    }
                }
            }
            
            cols.push_back(cd);

            if (i < tokens.size() && tokens[i] == ",") {
                i++;
            }
        }
    } catch (...) {
        callback("[Error] Critical syntax error while parsing table columns.\n");
        return;
    }

    Result create_res = TableManager::createTable(res.path, TableSchema(tableName, cols));
    
    if (create_res.isOk()) {
        callback("[Success] Table '" + tableName + "' created successfully.\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(create_res) + "\n");
    }
}

void SQLParser::handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 5) {
        callback("[Error] Syntax error. Expected: INSERT INTO [table] (cols) VALUE (vals);\n");
        return;
    }

    Result res = hm.resolveTablePath(tokens[2]);

    if (!res.isOk()) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return;
    }

    // 2. Читаем заголовок таблицы с диска для получения схемы
    TableHeader header;
    if (!Pager(res.path).read_page(0, &header).isOk()) {
        callback("[Error] Failed to read table schema from disk.\n");
        return;
    }

    // 3. Выделяем колонки и значения из токенов запроса
    std::vector<std::string> targetCols;
    size_t valStart = findValueStartIndex(tokens, targetCols);
    auto rawValues = collectValuesFromTokens(tokens, valStart);

    // 4. Подготавливаем объект Row и проверяем констрейнты (типы, NOT NULL, DEFAULT)
    Row finalRow(header.column_count, Value());

    if (prepareAndValidateRow(finalRow, header, targetCols, rawValues, callback)) {
        
        // 5. Пытаемся физически вставить строку
        Result insert_res = TableManager::insertRow(res.path, finalRow);
        
        if (insert_res.isOk()) {
            callback("[Success] " + insert_res.details + "\n");
        } else {
            callback("[Error] " + ErrorUtils::formatMessage(insert_res) + "\n");
        }
    }
}

void SQLParser::handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    std::string tableName;
    std::vector<std::string> selectedCols;
    std::vector<AggregateRequest> aggs;
    std::map<std::string, std::string> aliases;
    size_t fromIdx = 0;

    // 1. Поиск ключевого слова FROM и имени таблицы
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == "FROM") { 
            fromIdx = i; 
            if (i + 1 < tokens.size()) {
                tableName = tokens[i+1]; 
            }
            break; 
        }
    }

    // 2. Разбор списка выбираемых колонок, агрегатов и алиасов
    if (tokens.size() > 1 && tokens[1] != "*") {
        for (size_t i = 1; i < fromIdx; ++i) {
            std::string token = toUpper(tokens[i]);
            
            // Логика агрегатов (Задание №12)
            if (token == "SUM" || token == "COUNT" || token == "AVG") {
                AggregateType type = AggregateType::NONE;
                if (token == "SUM") type = AggregateType::SUM;
                else if (token == "COUNT") type = AggregateType::COUNT;
                else if (token == "AVG") type = AggregateType::AVG;
                
                // Проверка синтаксиса агрегата: SUM ( col )
                if (i + 3 < fromIdx && tokens[i+1] == "(" && tokens[i+3] == ")") {
                    aggs.push_back({type, tokens[i+2]});
                    i += 3;
                    continue;
                }
            }

            if (tokens[i] == ",") continue;

            // Логика алиасов (AS)
            if (toUpper(tokens[i]) == "AS") {
                if (i + 1 < fromIdx) {
                    std::string alias = tokens[i+1];
                    if (alias.front() == '"') alias = alias.substr(1, alias.size() - 2);
                    if (!selectedCols.empty()) aliases[selectedCols.back()] = alias;
                    i++;
                }
            } else {
                selectedCols.push_back(tokens[i]);
            }
        }
    }

    // 3. Выполнение запроса
    auto res = hm.resolveTablePath(tableName);

    if (res.isOk()) {
        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);
        
        Result select_res = TableManager::executeSelect(res.path, tree.get(), selectedCols, aliases, aggs, callback);
        
        if (!select_res.isOk()) {
            callback("[Error] " + ErrorUtils::formatMessage(select_res) + "\n");
        }
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }
}

void SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    // 1. Проверка синтаксиса: DELETE FROM [table_name]
    if (tokens.size() < 3) {
        callback("[Error] Syntax error. Expected: DELETE FROM [table_name] <WHERE condition>;\n");
        return;
    }

    // Пытаемся разрешить путь к таблице
    auto res = hm.resolveTablePath(tokens[2]);

    if (res.isOk()) {
        // Выделяем токены после слова WHERE и строим абстрактное дерево условий
        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);

        // Вызываем движок для физического удаления записей
        Result del_res = TableManager::executeDelete(res.path, tree.get());

        if (del_res.isOk()) {
            callback("[Success] " + del_res.details + "\n");
        } else {
            callback("[Error] " + ErrorUtils::formatMessage(del_res) + "\n");
        }
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }
}

void SQLParser::handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    // 1. Проверка синтаксиса: UPDATE [table] SET [col] = [val] <WHERE ...>
    if (tokens.size() < 6) {
        callback("[Error] Syntax error. Expected: UPDATE [table] SET [col] = [val] <WHERE condition>;\n");
        return;
    }

    // Разрешаем путь к таблице (в UPDATE имя таблицы обычно идет вторым токеном)
    auto res = hm.resolveTablePath(tokens[1]);

    if (res.isOk()) {
        // Извлекаем условия фильтрации после слова WHERE
        auto whereTokens = getWhereTokens(tokens);
        auto tree = buildExpressionTree(whereTokens);

        // Вызываем движок для обновления данных. 
        // Логика: tokens[3] - имя колонки, tokens[5] - новое значение.
        Result upd_res = TableManager::executeUpdate(res.path, tree.get(), tokens[3], tokens[5]);

        if (upd_res.isOk()) {
            // Выводим подтверждение об успехе
            callback("[Success] " + upd_res.details + "\n");
        } else {
            // Выводим системную ошибку (например, нарушение констрейнтов при обновлении)
            callback("[Error] " + ErrorUtils::formatMessage(upd_res) + "\n");
        }
    } else {
        // Если таблица не найдена
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }
}

size_t SQLParser::findValueStartIndex(const std::vector<std::string>& tokens, std::vector<std::string>& outColNames) const {
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

std::vector<std::string> SQLParser::collectValuesFromTokens(const std::vector<std::string>& tokens, size_t startIdx) const {
    std::vector<std::string> values;
    if (startIdx == 0) return values;
    for (size_t i = startIdx; i < tokens.size() && tokens[i] != ")"; ++i) {
        if (tokens[i] != ",") values.push_back(tokens[i]);
    }
    return values;
}

bool SQLParser::prepareAndValidateRow(Row& outRow, const TableHeader& header, 
                                     const std::vector<std::string>& targetCols, 
                                     const std::vector<std::string>& rawValues,
                                     OutputCallback callback) {
    
    // 1. Проверка количества (если пользователь не указал колонки явно, например: INSERT INTO table VALUE (1,2))
    if (targetCols.empty() && rawValues.size() != header.column_count) {
        callback("[Error] Column count mismatch. Expected " + std::to_string(header.column_count) + ".\n");
        return false;
    }

    // 2. Наполнение Row данными (с использованием нашего parseLiteral для типизации)
    if (!targetCols.empty()) {
        // Случай: INSERT INTO table (col_a, col_c) VALUE (10, "test")
        for (size_t i = 0; i < targetCols.size() && i < rawValues.size(); ++i) {
            int cIdx = -1;
            // Ищем индекс колонки в заголовке таблицы по её имени
            for (uint32_t c = 0; c < header.column_count; ++c) {
                if (std::string(header.columns[c].name) == targetCols[i]) { 
                    cIdx = (int)c; 
                    break; 
                }
            }
            // Если колонка найдена — парсим литерал и кладем в нужный слот Row
            if (cIdx != -1) {
                outRow[cIdx] = parseLiteral(rawValues[i]);
            } else {
                callback("[Error] Column '" + targetCols[i] + "' not found in table schema.\n");
                return false;
            }
        }
    } else {
        // Случай: INSERT INTO table VALUE (10, "val", 20) — заполняем по порядку
        for (size_t i = 0; i < rawValues.size() && i < header.column_count; ++i) {
            outRow[i] = parseLiteral(rawValues[i]);
        }
    }

    // 3. Реализация констрейнтов: Проверка NOT_NULL и применение DEFAULT (Задание №10)
    for (uint32_t i = 0; i < header.column_count; ++i) {
        // Если в слоте после парсинга осталось пустое (NULL) значение
        if (outRow[i].is_null) {
            // Если для колонки задано значение по умолчанию
            if (header.columns[i].has_default) {
                // Подставляем дефолтное значение из метаданных таблицы
                outRow[i] = parseLiteral(header.columns[i].default_val);
            } 
            // Если дефолта нет, а колонка помечена как NOT_NULL
            else if (header.columns[i].is_not_null) {
                callback("[Error] Constraint Violation: Column '" + std::string(header.columns[i].name) + "' is NOT_NULL.\n");
                return false;
            }
        }
    }
    
    return true; // Строка успешно валидирована и готова к физической вставке на диск
}
