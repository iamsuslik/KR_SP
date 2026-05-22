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
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { 
        return std::toupper(c); 
    });
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
    
    // Если это строка в кавычках
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return Value(token.substr(1, token.size() - 2));
    }
    
    // Попытка распарсить как число
    try {
        size_t pos;
        int val = std::stoi(token, &pos);
        if (pos == token.size()) return Value(val);
    } catch (...) {
        // Если stoi упал, значит это не число. 
        // В зависимости от контекста это либо идентификатор, либо ошибка.
    }

    return Value(token);
}

bool SQLParser::isValidIdentifier(const std::string& name) {
    // Регулярки — это хорошо, но можно добавить проверку на длину (ТЗ: имена ограничены)
    if (name.empty() || name.size() > MAX_NAME_LEN) return false;
    
    static const std::regex pattern("^[a-zA-Z_][a-zA-Z0-9_]*$");
    return std::regex_match(name, pattern);
}

int SQLParser::getPrecedence(const std::string& op) {
    if (op == "OR") return 1;
    if (op == "AND") return 2;
    // Все операторы сравнения имеют одинаковый приоритет
    if (op == "=" || op == "==" || op == ">" || op == "<" || op == "!=" || op == ">=" || op == "<=" || op == "LIKE") return 3;
    return 0;
}

// ВСПОМОГАТЕЛЬНЫЙ МЕТОД: Создание листа дерева (операнда)
std::shared_ptr<ExpressionNode> SQLParser::createLeaf(const std::string& token) {
    auto leaf = std::make_shared<ExpressionNode>();
    leaf->is_op = false;
    
    Value parsed = parseLiteral(token);
    // Сохраняем имя колонки или строковое представление значения
    leaf->column = (parsed.type == DataType::STR) ? parsed.str_val : std::to_string(parsed.int_val);
    return leaf;
}

// ВСПОМОГАТЕЛЬНЫЙ МЕТОД: Связывание оператора с операндами
void SQLParser::applyOperator(std::stack<std::shared_ptr<ExpressionNode>>& values, std::stack<std::string>& ops) {
    if (ops.empty() || values.size() < 2) return; // Защита от кривого SQL
    
    std::string op = ops.top(); ops.pop();
    auto right = values.top(); values.pop();
    auto left = values.top(); values.pop();

    auto node = std::make_shared<ExpressionNode>();
    node->op = op;

    if (op == "AND" || op == "OR") {
        node->is_op = true;
        node->left = left;
        node->right = right;
    } else {
        // Логика сравнения (age = 20)
        node->is_op = false;
        node->column = left->column; // Слева — имя колонки
        node->val1 = right->column;  // Справа — значение
        node->val1_parsed = parseLiteral(right->column); // Сразу парсим для движка БД
    }
    values.push(node);
}

// ОСНОВНОЙ МЕТОД: Построение дерева
std::shared_ptr<ExpressionNode> SQLParser::buildExpressionTree(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return nullptr;

    std::stack<std::shared_ptr<ExpressionNode>> values;
    std::stack<std::string> ops;

    for (const auto& token : tokens) {
        if (token.empty()) continue;

        std::string upToken = toUpper(token);
        
        if (upToken == "(") {
            ops.push(upToken);
        } 
        else if (upToken == ")") {
            while (!ops.empty() && ops.top() != "(") {
                applyOperator(values, ops);
            }
            if (!ops.empty()) ops.pop(); // Выбрасываем открывающую скобку
        } 
        else if (getPrecedence(upToken) > 0) {
            // Если приоритет текущего оператора ниже или равен тому, что в стеке — выполняем старые
            while (!ops.empty() && ops.top() != "(" && getPrecedence(ops.top()) >= getPrecedence(upToken)) {
                applyOperator(values, ops);
            }
            ops.push(upToken);
        } 
        else {
            // Это операнд (колонка или число/строка)
            values.push(createLeaf(token));
        }
    }

    // Довыполняем всё, что осталось в стеке
    while (!ops.empty()) {
        if (ops.top() == "(") { ops.pop(); continue; } // Защита от лишних скобок
        applyOperator(values, ops);
    }

    return values.empty() ? nullptr : values.top();
}

std::vector<std::string> SQLParser::tokenize(const std::string& query) const { 
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;

    // Лямбда для "сброса" текущего слова в список токенов
    auto pushCurrent = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };

    for (size_t i = 0; i < query.length(); ++i) {
        char c = query[i];
        
        // 1. Работа с кавычками (строковые литералы)
        if (c == '"') {
            inQuotes = !inQuotes;
            current += c;
            if (!inQuotes) pushCurrent(); // Кавычка закрылась — токен готов
            continue;
        }

        // Если мы внутри кавычек, просто копируем всё подряд
        if (inQuotes) {
            current += c;
            continue;
        }

        // 2. Работа с разделителями
        if (std::isspace(static_cast<unsigned char>(c))) {
            pushCurrent();
        } 
        else if (c == ',' || c == '(' || c == ')' || c == ';') {
            pushCurrent();
            // Точку с запятой обычно не добавляют в токены, она просто маркер конца
            if (c != ';') {
                tokens.push_back(std::string(1, c));
            }
        } 
        // 3. Работа с операторами (включая составные ==, !=, <=, >=)
        else if (c == '=' || c == '<' || c == '>' || c == '!') {
            pushCurrent();
            std::string op(1, c);
            
            // Заглядываем вперед: не идет ли следом '='?
            if (i + 1 < query.length() && query[i + 1] == '=') { 
                op += "="; 
                i++; // Пропускаем следующий символ, так как он уже часть оператора
            }
            tokens.push_back(op);
        } 
        // 4. Обычные символы (имена таблиц, колонок, ключевые слова)
        else {
            current += c;
        }
    }
    
    pushCurrent(); // Не забываем последний токен
    return tokens;
}

// 1. Помощник для проверки регистра (вынесли из основного метода)
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

// 2. Вынесли логику DROP, чтобы не раздувать основной метод
Result SQLParser::handleDrop(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 3) {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Incomplete DROP command");
    }

    std::string sub = toUpper(tokens[1]);
    Result final_res;

    if (sub == "TABLE") {
        auto res = hm.resolveTablePath(tokens[2]);
        if (!res.isOk()) {
            callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
            return res;
        }
        final_res = TableManager::dropTable(res.path);
    } 
    else if (sub == "DATABASE") {
        final_res = hm.dropDatabase(tokens[2]);
    } 
    else {
        return Result::Error(StatusCode::SYNTAX_ERROR, "Unknown DROP target: " + sub);
    }

    callback(final_res.isOk() ? "[Success] " + final_res.details + "\n" : "[Error] " + ErrorUtils::formatMessage(final_res) + "\n");
    return final_res;
}

// 3. ОБНОВЛЕННЫЙ ОСНОВНОЙ МЕТОД
Result SQLParser::process(const std::string& query, HierarchyManager& hm, OutputCallback callback) {
    auto tokens = tokenize(query);
    if (tokens.empty()) return Result::Success();
    if (tokens[0].front() == '[') return Result::Success(); // Игнорируем комментарии

    // Шаг 1: Валидация регистра
    Result case_res = validateTokenCase(tokens, callback);
    if (!case_res.isOk()) return case_res;

    std::string cmd = toUpper(tokens[0]);

    // Шаг 2: Чистая диспетчеризация
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

    // Если ничего не подошло
    std::string err = "[Error] Unknown command: " + tokens[0] + "\n";
    callback(err);
    return Result::Error(StatusCode::SYNTAX_ERROR, err);
}

// Вспомогательный метод для выделения токенов после WHERE
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

    // Вызываем логику создания БД
    Result res = hm.createDatabase(tokens[2]);
    
    if (res.isOk()) {
        callback("[Success] " + res.details + "\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }

    return res; // Теперь Даша узнает, создалась ли папка на диске
}

Result SQLParser::handleUse(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 2) {
        std::string err = "[Error] Syntax error. Expected: USE [database_name];\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    // Переключаем контекст в HierarchyManager
    Result res = hm.useDatabase(tokens[1]);
    
    if (res.isOk()) {
        callback("[Success] " + res.details + "\n");
    } else {
        // Если БД не существует, HierarchyManager вернет DATABASE_NOT_FOUND
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
    }

    return res;
}

// ВСПОМОГАТЕЛЬНЫЙ МЕТОД: Парсинг списка колонок (col type flags, ...)
Result SQLParser::parseColumnDefinitions(const std::vector<std::string>& tokens, size_t& i, std::vector<ColumnDef>& outCols) {
    while (i < tokens.size() && tokens[i] != ")") {
        if (i + 1 >= tokens.size()) return Result::Error(StatusCode::SYNTAX_ERROR, "Unexpected end of column list");

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
                cd.has_default = true;
                if (i < tokens.size()) {
                    cd.default_value = tokens[i++]; // Значение по умолчанию
                }
            }
        }
        
        outCols.push_back(cd);

        if (i < tokens.size() && tokens[i] == ",") {
            i++; // Пропускаем запятую перед следующей колонкой
        }
    }
    return Result::Success();
}

// ОСНОВНОЙ МЕТОД
Result SQLParser::handleCreateTable(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 6) {
        std::string err = "[Error] Syntax error. Expected: CREATE TABLE table_name (col1 type [flags], ...);\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    std::string tableName = tokens[2];
    Result res = hm.resolveTablePath(tableName);

    // 1. Проверка контекста базы данных
    if (res.code == StatusCode::DATABASE_NOT_FOUND) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return res;
    }

    // 2. Проверка существования таблицы
    if (res.isOk()) {
        std::string err = "Table '" + tableName + "' already exists.\n";
        callback("[Error] " + err);
        return Result::Error(StatusCode::ALREADY_EXISTS, err); 
    }

    // 3. Парсинг схемы (используем наш новый метод)
    std::vector<ColumnDef> cols;
    size_t index = 4; // Сразу после '('
    Result parse_res = parseColumnDefinitions(tokens, index, cols);
    
    if (!parse_res.isOk()) {
        callback("[Error] " + parse_res.details + "\n");
        return parse_res;
    }

    // 4. Физическое создание таблицы через движок
    Result create_res = TableManager::createTable(res.path, TableSchema(tableName, cols));
    
    if (create_res.isOk()) {
        callback("[Success] Table '" + tableName + "' created successfully.\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(create_res) + "\n");
    }

    return create_res;
}

Result SQLParser::handleInsert(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    if (tokens.size() < 5) {
        std::string err = "[Error] Syntax error. Expected: INSERT INTO [table] (cols) VALUE (vals);\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    // 1. Находим путь к таблице
    Result res = hm.resolveTablePath(tokens[2]);
    if (!res.isOk()) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return res;
    }

    // 2. Читаем заголовок таблицы с диска
    TableHeader header;
    Result page_res = Pager(res.path).read_page(0, &header);
    if (!page_res.isOk()) {
        callback("[Error] Failed to read table schema from disk.\n");
        return page_res; // Возвращаем IO_ERROR
    }

    // 3. Выделяем колонки и значения из токенов
    std::vector<std::string> targetCols;
    size_t valStart = findValueStartIndex(tokens, targetCols);
    auto rawValues = collectValuesFromTokens(tokens, valStart);

    // 4. Подготавливаем объект Row и проверяем констрейнты
    Row finalRow(header.column_count, Value());

    // Если валидация не прошла (например, нарушение NOT_NULL)
    if (!prepareAndValidateRow(finalRow, header, targetCols, rawValues, callback)) {
        // prepareAndValidateRow уже вывел текст ошибки через callback
        return Result::Error(StatusCode::INVALID_VALUE, "Row validation failed");
    }
        
    // 5. Пытаемся физически вставить строку через движок
    Result insert_res = TableManager::insertRow(res.path, finalRow);
    
    if (insert_res.isOk()) {
        callback("[Success] " + insert_res.details + "\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(insert_res) + "\n");
    }

    return insert_res;
}
// 1. Помощник для поиска ключевых слов
size_t SQLParser::findKeyword(const std::vector<std::string>& tokens, const std::string& keyword) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (toUpper(tokens[i]) == keyword) return i;
    }
    return std::string::npos;
}

// 2. Вынесли логику парсинга колонок, агрегатов и алиасов
Result SQLParser::parseProjection(const std::vector<std::string>& tokens, size_t fromIdx, 
                                 std::vector<std::string>& selectedCols, 
                                 std::vector<AggregateRequest>& aggs, 
                                 std::map<std::string, std::string>& aliases) {
    if (fromIdx == 1 && tokens[1] == "*") return Result::Success();

    for (size_t i = 1; i < fromIdx; ++i) {
        std::string token = toUpper(tokens[i]);
        if (token == ",") continue;

        // Логика агрегатов (SUM, COUNT, AVG)
        if (token == "SUM" || token == "COUNT" || token == "AVG") {
            if (i + 3 < fromIdx && tokens[i+1] == "(" && tokens[i+3] == ")") {
                AggregateType type = (token == "SUM") ? AggregateType::SUM : 
                                    (token == "COUNT") ? AggregateType::COUNT : AggregateType::AVG;
                aggs.push_back({type, tokens[i+2]});
                i += 3; continue;
            }
        }

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
    return Result::Success();
}

// 3. ОСНОВНОЙ МЕТОД
Result SQLParser::handleSelect(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    // Шаг 1: Ищем FROM
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

    // Шаг 2: Парсим то, ЧТО выбираем
    Result proj_res = parseProjection(tokens, fromIdx, selectedCols, aggs, aliases);
    if (!proj_res.isOk()) return proj_res;

    // Шаг 3: Разрешаем путь к таблице
    auto res = hm.resolveTablePath(tableName);
    if (!res.isOk()) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return res;
    }

    // Шаг 4: Строим дерево условий WHERE
    auto whereTokens = getWhereTokens(tokens);
    auto tree = buildExpressionTree(whereTokens);
    
    // Шаг 5: Выполняем через движок
    Result select_res = TableManager::executeSelect(res.path, tree.get(), selectedCols, aliases, aggs, callback);
    
    if (!select_res.isOk()) {
        callback("[Error] " + ErrorUtils::formatMessage(select_res) + "\n");
    }

    return select_res;
}

Result SQLParser::handleDelete(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    // 1. Проверка синтаксиса: DELETE FROM [table_name]
    // Мы проверяем не только количество, но и наличие слова FROM
    if (tokens.size() < 3 || toUpper(tokens[1]) != "FROM") {
        std::string err = "[Error] Syntax error. Expected: DELETE FROM [table_name] <WHERE condition>;\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    // 2. Пытаемся разрешить путь к таблице (токен под индексом 2)
    auto res = hm.resolveTablePath(tokens[2]);
    if (!res.isOk()) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return res;
    }

    // 3. Работа с условиями
    // Выделяем токены после слова WHERE и строим абстрактное дерево условий
    auto whereTokens = getWhereTokens(tokens);
    auto tree = buildExpressionTree(whereTokens);

    // 4. Вызываем движок для физического удаления записей
    Result del_res = TableManager::executeDelete(res.path, tree.get());

    // 5. Формируем ответ пользователю
    if (del_res.isOk()) {
        callback("[Success] " + del_res.details + "\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(del_res) + "\n");
    }

    return del_res;
}

Result SQLParser::handleUpdate(const std::vector<std::string>& tokens, HierarchyManager& hm, OutputCallback callback) {
    // 1. Проверка синтаксиса: UPDATE [table] SET [col] = [val] <WHERE ...>
    // Проверяем наличие ключевого слова SET на правильной позиции (индекс 2)
    if (tokens.size() < 6 || toUpper(tokens[2]) != "SET") {
        std::string err = "[Error] Syntax error. Expected: UPDATE [table] SET [col] = [val] <WHERE condition>;\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    // 2. Проверка оператора присваивания в блоке SET
    if (tokens[4] != "=") {
        std::string err = "[Error] Syntax error. Expected '=' after column name in SET clause.\n";
        callback(err);
        return Result::Error(StatusCode::SYNTAX_ERROR, err);
    }

    // 3. Разрешаем путь к таблице (токен под индексом 1)
    auto res = hm.resolveTablePath(tokens[1]);
    if (!res.isOk()) {
        callback("[Error] " + ErrorUtils::formatMessage(res) + "\n");
        return res;
    }

    // 4. Извлекаем условия фильтрации после слова WHERE
    auto whereTokens = getWhereTokens(tokens);
    auto tree = buildExpressionTree(whereTokens);

    // 5. Вызываем движок для обновления данных. 
    // Логика: tokens[3] - имя колонки, tokens[5] - новое значение.
    Result upd_res = TableManager::executeUpdate(res.path, tree.get(), tokens[3], tokens[5]);

    // 6. Формируем ответ
    if (upd_res.isOk()) {
        callback("[Success] " + upd_res.details + "\n");
    } else {
        callback("[Error] " + ErrorUtils::formatMessage(upd_res) + "\n");
    }

    return upd_res;
}

// 1. Поиск индекса, где начинаются сами значения (после слова VALUE)
size_t SQLParser::findValueStartIndex(const std::vector<std::string>& tokens, std::vector<std::string>& outColNames) const {
    size_t i = 3; // Начинаем после "INSERT INTO table_name"

    // Обработка списка колонок: (col1, col2, ...)
    if (i < tokens.size() && tokens[i] == "(") {
        i++; // Пропускаем открывающую скобку
        while (i < tokens.size() && tokens[i] != ")") {
            if (tokens[i] != ",") {
                outColNames.push_back(tokens[i]);
            }
            i++;
        }
        if (i < tokens.size()) i++; // Пропускаем закрывающую скобку
    }

    // Ищем ключевое слово VALUE (пропускаем всё до него)
    while (i < tokens.size() && toUpper(tokens[i]) != "VALUE") {
        i++;
    }
    
    // После слова VALUE должна идти открывающая скобка значений: VALUE ( v1, v2 )
    // Поэтому возвращаем индекс элемента СРАЗУ после '('
    if (i + 1 < tokens.size() && tokens[i+1] == "(") {
        return i + 2; 
    }

    return std::string::npos; // Возвращаем специальную метку "не найдено"
}

// 2. Сбор значений в массив строк
std::vector<std::string> SQLParser::collectValuesFromTokens(const std::vector<std::string>& tokens, size_t startIdx) const {
    std::vector<std::string> values;
    
    // Если индекс невалиден (npos), возвращаем пустой массив
    if (startIdx == std::string::npos || startIdx >= tokens.size()) {
        return values;
    }

    // Собираем токены, пока не встретим закрывающую скобку запроса
    for (size_t i = startIdx; i < tokens.size() && tokens[i] != ")"; ++i) {
        if (tokens[i] != ",") {
            values.push_back(tokens[i]);
        }
    }
    return values;
}

// 1. Помощник: ищет порядковый номер колонки по её имени
int SQLParser::findColumnIndex(const TableHeader& header, const std::string& colName) const {
    for (uint32_t c = 0; c < header.column_count; ++c) {
        if (std::string(header.columns[c].name) == colName) return (int)c;
    }
    return -1;
}

// 2. Помощник: проверяет NOT_NULL и подставляет DEFAULT (Задание №10)
bool SQLParser::applyConstraints(Row& row, const TableHeader& header, OutputCallback callback) const {
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (row[i].is_null) {
            // Если есть значение по умолчанию — подставляем его
            if (header.columns[i].has_default) {
                row[i] = parseLiteral(header.columns[i].default_val);
            } 
            // Если дефолта нет, а колонка NOT_NULL — это ошибка
            else if (header.columns[i].is_not_null) {
                callback("[Error] Constraint Violation: Column '" + std::string(header.columns[i].name) + "' is NOT_NULL.\n");
                return false;
            }
        }
    }
    return true;
}

// 3. ОСНОВНОЙ МЕТОД
bool SQLParser::prepareAndValidateRow(Row& outRow, const TableHeader& header, 
                                     const std::vector<std::string>& targetCols, 
                                     const std::vector<std::string>& rawValues,
                                     OutputCallback callback) {
    
    // Шаг 1: Проверка количества данных
    if (targetCols.empty() && rawValues.size() != header.column_count) {
        callback("[Error] Column count mismatch. Expected " + std::to_string(header.column_count) + " values.\n");
        return false;
    }

    // Шаг 2: Маппинг данных в объект Row
    if (!targetCols.empty()) {
        // Случай: INSERT INTO (col1, col3) VALUE (val1, val3)
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
        // Случай: INSERT INTO table VALUE (val1, val2, val3...)
        for (size_t i = 0; i < rawValues.size() && i < header.column_count; ++i) {
            outRow[i] = parseLiteral(rawValues[i]);
        }
    }

    // Шаг 3: Применение констрейнтов (NOT NULL, DEFAULT)
    return applyConstraints(outRow, header, callback);
}
