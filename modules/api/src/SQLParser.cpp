#include "SQLParser.h"
#include "DbException.h"
#include "Statements.h"
#include "AsyncManager.h"
#include "ErrorUtils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <unordered_set>
#include <stdexcept>

std::string SQLParser::toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

bool SQLParser::isValidIdentifier(const std::string& name) {
    if (name.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    }
    return true;
}

void SQLParser::validateIdentifier(const std::string& name,
                                   const std::string& context) const {
    if (!isValidIdentifier(name))
        throw DbException(StatusCode::SYNTAX_ERROR,
                          "Недопустимое имя '" + name + "' в контексте: " + context +
                          ". Допустимы латинские буквы, цифры и '_'; не начинается с цифры.");
}

Value SQLParser::parseLiteral(const std::string& token) {
    if (token.empty() || toUpper(token) == "NULL")
        return Value();

    if (token.size() >= 2 && token.front() == '"' && token.back() == '"')
        return Value(token.substr(1, token.size() - 2));

    int int_val = 0;
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), int_val);
    if (ec == std::errc() && ptr == token.data() + token.size())
        return Value(int_val);

    return Value(token);
}


TokenType SQLParser::getTokenType(const std::string& value) const {
    if (value.empty()) return TokenType::IDENTIFIER;

    static const std::unordered_set<std::string> kKeywords = {
        "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUE", "VALUES",
        "CREATE", "TABLE", "DATABASE", "DROP", "USE", "UPDATE", "SET",
        "DELETE", "AND", "OR", "NOT", "BETWEEN", "LIKE",
        "INT", "STR",
        "NOT_NULL", "INDEXED", "DEFAULT",
        "AS", "NULL",
        "SUM", "COUNT", "AVG",
        "AUTH", "USER", "ALTER", "GRANT", "REVOKE",
        "GROUP", "PERMISSION", "PERMISSIONS",
        "GET", "REVERT", "TO", "ADD", "REMOVE"
    };

    const std::string up = toUpper(value);
    if (kKeywords.count(up)) return TokenType::KEYWORD;
    if (value.front() == '"')return TokenType::STRING_LITERAL;
    
    if (std::isdigit(static_cast<unsigned char>(value[0])) ||(value[0] == '-' && value.size() > 1 && std::isdigit(static_cast<unsigned char>(value[1])))){
        return TokenType::NUMBER;
    }

    return TokenType::IDENTIFIER;
}

std::vector<Token> SQLParser::lex(const std::string& query) {
    std::vector<Token> result;
    result.reserve(64);

    std::string current;
    bool inQuotes = false;

    auto pushToken = [&]() {
        if (current.empty()) return;
        result.push_back({getTokenType(current), current});
        current.clear();
    };

    for (std::size_t i = 0; i < query.size(); ++i) {
        const char c = query[i];

        if (c == '"') {
            if (inQuotes) {
                current += c;
                pushToken();
                inQuotes = false;
            } else {
                pushToken();
                inQuotes = true;
                current += c;
            }
            continue;
        }
        if (inQuotes) { current += c; continue; }

        if (std::isspace(static_cast<unsigned char>(c))) { pushToken(); continue; }

        if (c == ',') { pushToken(); result.push_back({TokenType::COMMA,","}); continue; }
        if (c == '(') { pushToken(); result.push_back({TokenType::LPAREN,"("}); continue; }
        if (c == ')') { pushToken(); result.push_back({TokenType::RPAREN,")"}); continue; }
        if (c == ';') { pushToken(); result.push_back({TokenType::SEMICOLON, ";"}); continue; }
        if (c == '.') { pushToken(); result.push_back({TokenType::DOT,"."}); continue; }
        if (c == '*') { pushToken(); result.push_back({TokenType::STAR, "*"}); continue; }

        if (c == '=' || c == '<' || c == '>' || c == '!') {
            pushToken();
            std::string op(1, c);
            if (i + 1 < query.size() && query[i + 1] == '=') {
                op += '=';
                ++i;
            }
            result.push_back({TokenType::OPERATOR, op});
            continue;
        }

        current += c;
    }
    pushToken();
    result.push_back({TokenType::END_OF_FILE, ""});
    return result;
}

Token SQLParser::peek() const {
    if (pos < token_stream.size()) return token_stream[pos];
    return {TokenType::END_OF_FILE, ""};
}

Token SQLParser::peekAhead(std::size_t offset) const {
    const std::size_t idx = pos + offset;
    if (idx < token_stream.size()) return token_stream[idx];
    return {TokenType::END_OF_FILE, ""};
}

Token SQLParser::consume() {
    if (pos < token_stream.size()) return token_stream[pos++];
    return {TokenType::END_OF_FILE, ""};
}

bool SQLParser::match(TokenType type, const std::string& val) {
    const Token t = peek();
    if (t.type != type) return false;
    if (!val.empty() && toUpper(t.value) != toUpper(val)) return false;
    consume();
    return true;
}

Token SQLParser::expect(TokenType type, const std::string& errMsg) {
    const Token t = peek();
    if (t.type != type)
        throw DbException(StatusCode::SYNTAX_ERROR,
                          errMsg + " (получено: '" + t.value + "')");
    return consume();
}

Result SQLParser::process(const std::string& query,
                          HierarchyManager& hm,
                          int client_fd,
                          OutputCallback cb) {
    try {
        if (g_async_manager) {
            const std::string active_db = g_async_manager->get_session_db(client_fd);
            if (!active_db.empty()) { auto _r2 = hm.useDatabase(active_db); (void)_r2; }
        }

        token_stream = lex(query);
        pos = 0;

        if (token_stream.empty() || peek().type == TokenType::END_OF_FILE)
            return Result::Success();

        for (const auto& tok : token_stream) {
            if (tok.type != TokenType::KEYWORD) continue;
            bool hasUpper = false, hasLower = false;
            for (char c : tok.value) {
                if (std::isupper(static_cast<unsigned char>(c))) hasUpper = true;
                if (std::islower(static_cast<unsigned char>(c))) hasLower = true;
            }
            if (hasUpper && hasLower)
                return Result::Error(StatusCode::SYNTAX_ERROR,
                                     "Запрещён смешанный регистр в ключевом слове: '" +
                                     tok.value + "'");
        }

        auto stmt = parseStatement();
        if (!stmt)
            return Result::Error(StatusCode::SYNTAX_ERROR, "Пустой запрос");

        return stmt->execute(hm, client_fd, cb);

    } catch (const DbException& e) {
        return Result::Error(e.code(), e.what());
    } catch (const std::exception& e) {
        return Result::Error(StatusCode::INTERNAL_ERROR, e.what());
    }
}

std::unique_ptr<Statement> SQLParser::parseStatement() {
    const Token t = peek();
    if (t.type == TokenType::END_OF_FILE) return nullptr;

    const std::string cmd = toUpper(t.value);

    if (cmd == "SELECT") return parseSelect();
    if (cmd == "INSERT") return parseInsert();
    if (cmd == "UPDATE") return parseUpdate();
    if (cmd == "DELETE") return parseDelete();
    if (cmd == "CREATE") return parseCreate();
    if (cmd == "DROP")   return parseDrop();
    if (cmd == "USE")    return parseUse();
    if (cmd == "AUTH")   return parseAuth();
    if (cmd == "ALTER")  return parseAlter();
    if (cmd == "GET")    return parseGet();
    if (cmd == "REVERT") return parseRevert();

    throw DbException(StatusCode::SYNTAX_ERROR,
                      "Неизвестная команда: '" + t.value + "'");
}

std::unique_ptr<Statement> SQLParser::parseSelect() {
    consume();

    std::vector<std::string>              selectedCols;
    std::vector<AggregateRequest>         aggs;
    std::map<std::string, std::string>    aliases;

    const Result projRes = parseProjection(selectedCols, aggs, aliases);
    if (!projRes.isOk())
        throw DbException(projRes.code, projRes.details);

    expect(TokenType::KEYWORD, "Ожидалось ключевое слово FROM");

    std::string tableName = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы").value;
    validateIdentifier(tableName, "FROM");

    if (peek().type == TokenType::DOT) {
        consume();
        const std::string part2 = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
        validateIdentifier(part2, "FROM (table part)");
        tableName = tableName + "." + part2;
    }

    std::unique_ptr<ASTNode> whereClause;
    if (match(TokenType::KEYWORD, "WHERE"))
        whereClause = parseExpression();

    match(TokenType::SEMICOLON);

    return std::make_unique<SelectStatement>(
        tableName, selectedCols, std::move(whereClause), aggs, aliases);
}

Result SQLParser::parseProjection(std::vector<std::string>& selectedCols,
                                  std::vector<AggregateRequest>& aggs,
                                  std::map<std::string, std::string>& aliases) {
    if (match(TokenType::STAR)) return Result::Success();

    auto parseOneColumn = [&]() {
        const Token t  = peek();
        const std::string up = toUpper(t.value);

        if (up == "SUM" || up == "COUNT" || up == "AVG") {
            consume();
            expect(TokenType::LPAREN, "Ожидалось '(' после агрегатной функции " + up);

            std::string arg;
            if (up == "COUNT" && match(TokenType::STAR)) {
                arg = "*";
            } else {
                arg = expect(TokenType::IDENTIFIER,
                             "Ожидалось имя колонки внутри " + up + "(...)").value;
                validateIdentifier(arg, up + "(...)");
            }
            expect(TokenType::RPAREN, "Ожидалось ')' после аргумента " + up);

            std::string alias = up + "(" + arg + ")";
            if (match(TokenType::KEYWORD, "AS"))
                alias = expect(TokenType::IDENTIFIER, "Ожидалось имя псевдонима").value;

            AggregateType type =
                (up == "SUM")   ? AggregateType::SUM :
                (up == "AVG")   ? AggregateType::AVG :
                                  AggregateType::COUNT;
            aggs.push_back({type, arg, alias});

        } else {
            const std::string name =
                expect(TokenType::IDENTIFIER, "Ожидалось имя колонки").value;
            validateIdentifier(name, "SELECT-список");

            std::string alias = name;
            if (match(TokenType::KEYWORD, "AS")) {
                alias = expect(TokenType::IDENTIFIER, "Ожидалось имя псевдонима").value;
                validateIdentifier(alias, "AS-псевдоним");
            }
            selectedCols.push_back(name);
            if (alias != name) aliases[name] = alias;
        }
    };

    parseOneColumn();
    while (match(TokenType::COMMA))
        parseOneColumn();

    return Result::Success();
}

std::unique_ptr<Statement> SQLParser::parseInsert() {
    consume();
    expect(TokenType::KEYWORD, "Ожидалось INTO после INSERT");

    std::string tableName = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы").value;
    validateIdentifier(tableName, "INSERT INTO");
    if (peek().type == TokenType::DOT) {
        consume();
        const std::string part2 = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
        validateIdentifier(part2, "INSERT INTO (table part)");
        tableName = tableName + "." + part2;
    }

    std::vector<std::string> columns;
    if (match(TokenType::LPAREN)) {
        do {
            const std::string col =
                expect(TokenType::IDENTIFIER, "Ожидалось имя колонки").value;
            validateIdentifier(col, "INSERT (column list)");
            columns.push_back(col);
        } while (match(TokenType::COMMA));
        expect(TokenType::RPAREN, "Ожидалось ')' после списка колонок");
    }

    if (match(TokenType::KEYWORD, "VALUE") || match(TokenType::KEYWORD, "VALUES")) {
        expect(TokenType::LPAREN, "Ожидалось '(' перед значениями");
        std::vector<std::string> values;
        do {
            values.push_back(consume().value);
        } while (match(TokenType::COMMA));
        expect(TokenType::RPAREN, "Ожидалось ')' после значений");
        match(TokenType::SEMICOLON);
        return std::make_unique<InsertStatement>(tableName, columns, values);
    }

    if (toUpper(peek().value) == "SELECT") {
        auto subquery = parseSelect();
        match(TokenType::SEMICOLON);
        return std::make_unique<InsertSelectStatement>(
            tableName, columns, std::move(subquery));
    }

    throw DbException(StatusCode::SYNTAX_ERROR,
                      "Ожидалось VALUE или SELECT после INSERT INTO");
}

std::unique_ptr<Statement> SQLParser::parseUpdate() {
    consume();

    std::string tableName = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы").value;
    validateIdentifier(tableName, "UPDATE");
    if (peek().type == TokenType::DOT) {
        consume();
        const std::string part2 = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
        validateIdentifier(part2, "UPDATE (table part)");
        tableName = tableName + "." + part2;
    }

    expect(TokenType::KEYWORD, "Ожидалось SET после имени таблицы в UPDATE");

    std::vector<std::pair<std::string, Value>> assignments;

    auto parseAssignment = [&]() {
        const std::string col =
            expect(TokenType::IDENTIFIER, "Ожидалось имя колонки в SET").value;
        validateIdentifier(col, "SET");
        expect(TokenType::OPERATOR, "Ожидался оператор '=' после имени колонки");
        assignments.emplace_back(col, parseLiteral(consume().value));
    };

    parseAssignment();
    while (match(TokenType::COMMA))
        parseAssignment();

    std::unique_ptr<ASTNode> whereClause;
    if (match(TokenType::KEYWORD, "WHERE"))
        whereClause = parseExpression();

    match(TokenType::SEMICOLON);
    return std::make_unique<UpdateStatement>(
        tableName, std::move(assignments), std::move(whereClause));
}

std::unique_ptr<Statement> SQLParser::parseDelete() {
    consume();
    expect(TokenType::KEYWORD, "Ожидалось FROM после DELETE");

    std::string tableName = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы").value;
    validateIdentifier(tableName, "DELETE FROM");
    if (peek().type == TokenType::DOT) {
        consume();
        const std::string part2 = expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
        validateIdentifier(part2, "DELETE FROM (table part)");
        tableName = tableName + "." + part2;
    }

    std::unique_ptr<ASTNode> whereClause;
    if (match(TokenType::KEYWORD, "WHERE"))
        whereClause = parseExpression();

    match(TokenType::SEMICOLON);
    return std::make_unique<DeleteStatement>(tableName, std::move(whereClause));
}

std::unique_ptr<Statement> SQLParser::parseUse() {
    consume();
    const std::string dbName = expect(TokenType::IDENTIFIER, "Ожидалось имя базы данных").value;
    validateIdentifier(dbName, "USE");
    match(TokenType::SEMICOLON);
    return std::make_unique<UseStatement>(dbName);
}

std::unique_ptr<Statement> SQLParser::parseCreate() {
    consume();
    const std::string target = toUpper(peek().value);

    if (target == "DATABASE") {
        consume();
        const std::string dbName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя базы данных").value;
        validateIdentifier(dbName, "CREATE DATABASE");
        match(TokenType::SEMICOLON);
        return std::make_unique<CreateDbStatement>(dbName);
    }

    if (target == "USER") {
        consume();
        const std::string user =
            expect(TokenType::IDENTIFIER, "Ожидалось имя пользователя").value;
        validateIdentifier(user, "CREATE USER");
        const std::string pass = consume().value;
        match(TokenType::SEMICOLON);
        return std::make_unique<CreateUserStatement>(user, pass);
    }

    if (target == "TABLE") {
        consume();

        std::string tableName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы").value;
        validateIdentifier(tableName, "CREATE TABLE");
        if (peek().type == TokenType::DOT) {
            consume();
            const std::string part2 =
                expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
            validateIdentifier(part2, "CREATE TABLE (table part)");
            tableName = tableName + "." + part2;
        }

        std::vector<ColumnDef> cols;
        const Result colRes = parseColumnDefinitions(cols);
        if (!colRes.isOk())
            throw DbException(colRes.code, colRes.details);

        if (cols.empty())
            throw DbException(StatusCode::SYNTAX_ERROR,
                              "CREATE TABLE: таблица должна иметь хотя бы одну колонку");

        match(TokenType::SEMICOLON);
        return std::make_unique<CreateTableStatement>(tableName, std::move(cols));
    }

    throw DbException(StatusCode::SYNTAX_ERROR,
                      "Ожидалось DATABASE, TABLE или USER после CREATE, получено: '" +
                      peek().value + "'");
}

Result SQLParser::parseColumnDefinitions(std::vector<ColumnDef>& outCols) {
    expect(TokenType::LPAREN, "Ожидалось '(' перед списком колонок");

    do {
        const std::string colName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя колонки").value;
        validateIdentifier(colName, "определение колонки");

        const std::string colTypeStr =
            toUpper(expect(TokenType::KEYWORD,
                           "Ожидался тип колонки INT или STR").value);
        if (colTypeStr != "INT" && colTypeStr != "STR")
            throw DbException(StatusCode::SYNTAX_ERROR,
                              "Неизвестный тип колонки '" + colTypeStr +
                              "'. Допустимы INT и STR.");

        ColumnDef cd(colName,
                     colTypeStr == "INT" ? DataType::INT : DataType::STR);

        bool parsing_flags = true;
        while (parsing_flags && peek().type == TokenType::KEYWORD) {
            const std::string flag = toUpper(peek().value);
            if (flag == "INDEXED") {
                consume();
                cd.is_indexed  = true;
                cd.is_not_null = true;
            } else if (flag == "NOT_NULL") {
                consume();
                cd.is_not_null = true;
            } else if (flag == "DEFAULT") {
                consume();
                const Token defTok = consume();
                cd.has_default   = true;
                cd.default_value = defTok.value;
            } else {
                parsing_flags = false;
            }
        }
        outCols.push_back(std::move(cd));
    } while (match(TokenType::COMMA));

    expect(TokenType::RPAREN, "Ожидалось ')' после списка колонок");
    return Result::Success();
}

std::unique_ptr<Statement> SQLParser::parseDrop() {
    consume();
    const std::string target = toUpper(peek().value);

    if (target == "DATABASE") {
        consume();
        const std::string dbName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя базы данных").value;
        validateIdentifier(dbName, "DROP DATABASE");
        match(TokenType::SEMICOLON);
        return std::make_unique<DropDbStatement>(dbName);
    }

    if (target == "TABLE") {
        consume();
        std::string tableName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы").value;
        validateIdentifier(tableName, "DROP TABLE");
        if (peek().type == TokenType::DOT) {
            consume();
            const std::string part2 =
                expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
            validateIdentifier(part2, "DROP TABLE (table part)");
            tableName = tableName + "." + part2;
        }
        match(TokenType::SEMICOLON);
        return std::make_unique<DropTableStatement>(tableName);
    }

    throw DbException(StatusCode::SYNTAX_ERROR,
                      "Ожидалось DATABASE или TABLE после DROP, получено: '" +
                      peek().value + "'");
}

std::unique_ptr<Statement> SQLParser::parseAuth() {
    consume();
    const std::string user =
        expect(TokenType::IDENTIFIER, "Ожидалось имя пользователя").value;
    const std::string pass = consume().value;
    match(TokenType::SEMICOLON);
    return std::make_unique<AuthStatement>(user, pass);
}

std::unique_ptr<Statement> SQLParser::parseGet() {
    consume();
    std::string guid;
    while (peek().type != TokenType::SEMICOLON &&
           peek().type != TokenType::END_OF_FILE) {
        guid += consume().value;
    }
    match(TokenType::SEMICOLON);
    if (guid.empty())
        throw DbException(StatusCode::SYNTAX_ERROR,
                          "GET: ожидался GUID задачи");
    return std::make_unique<GetTaskStatusStatement>(guid);
}

std::unique_ptr<Statement> SQLParser::parseRevert() {
    consume();

    std::string tableName =
        expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после REVERT").value;
    validateIdentifier(tableName, "REVERT");
    if (peek().type == TokenType::DOT) {
        consume();
        const std::string part2 =
            expect(TokenType::IDENTIFIER, "Ожидалось имя таблицы после '.'").value;
        validateIdentifier(part2, "REVERT (table part)");
        tableName = tableName + "." + part2;
    }

    std::string timestamp;
    while (peek().type != TokenType::SEMICOLON &&
           peek().type != TokenType::END_OF_FILE) {
        timestamp += consume().value;
    }
    match(TokenType::SEMICOLON);

    if (timestamp.empty())
        throw DbException(StatusCode::SYNTAX_ERROR,
                          "REVERT: ожидалась метка времени "
                          "(формат: yyyy.mm.dd-hh:mm:ss.msmsms)");

    return std::make_unique<RevertStatement>(tableName, timestamp);
}

std::unique_ptr<Statement> SQLParser::parseAlter() {
    consume();
    const std::string target = toUpper(peek().value);

    if (target == "USER") {
        consume();
        const std::string username =
            expect(TokenType::IDENTIFIER, "Ожидалось имя пользователя").value;
        validateIdentifier(username, "ALTER USER");

        if (match(TokenType::KEYWORD, "ADD")) {
            if (match(TokenType::KEYWORD, "TO")) {
                expect(TokenType::KEYWORD, "Ожидалось GROUP");
                const std::string dbName =
                    expect(TokenType::IDENTIFIER, "Ожидалось имя БД").value;
                validateIdentifier(dbName, "ADD TO GROUP (db)");
                const std::string groupName =
                    expect(TokenType::IDENTIFIER, "Ожидалось имя группы").value;
                validateIdentifier(groupName, "ADD TO GROUP (group)");
                match(TokenType::SEMICOLON);
                return std::make_unique<AlterUserAddToGroupStatement>(
                    username, dbName, groupName);
            }

            if (toUpper(peek().value) == "PERMISSION" ||
                toUpper(peek().value) == "PERMISSIONS") {
                consume();
                const std::string dbName =
                    expect(TokenType::IDENTIFIER, "Ожидалось имя БД").value;
                validateIdentifier(dbName, "ADD PERMISSION (db)");
                auto perms = parseIdentifierList();
                match(TokenType::SEMICOLON);
                return std::make_unique<AlterUserAddPermStatement>(
                    username, dbName, std::move(perms));
            }
        }
        throw DbException(StatusCode::SYNTAX_ERROR,
                          "ALTER USER: неизвестное действие");
    }

    if (target == "GROUP") {
        consume();
        const std::string groupName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя группы").value;
        validateIdentifier(groupName, "ALTER GROUP");

        if (match(TokenType::KEYWORD, "FROM")) {
            const bool isAdd = toUpper(peek().value) == "ADD";
            if (!isAdd && toUpper(peek().value) != "DELETE")
                throw DbException(StatusCode::SYNTAX_ERROR,
                                  "ALTER GROUP FROM: ожидалось ADD или DELETE");
            consume();

            if (toUpper(peek().value) != "PERMISSION" &&
                toUpper(peek().value) != "PERMISSIONS")
                throw DbException(StatusCode::SYNTAX_ERROR,
                                  "ALTER GROUP FROM ADD/DELETE: ожидалось PERMISSION");
            consume();

            const std::string dbName =
                expect(TokenType::IDENTIFIER, "Ожидалось имя БД").value;
            validateIdentifier(dbName, "ALTER GROUP (db)");
            auto perms = parseIdentifierList();
            match(TokenType::SEMICOLON);

            if (isAdd)
                return std::make_unique<AlterGroupAddPermStatement>(
                    groupName, dbName, std::move(perms));
            else
                return std::make_unique<AlterGroupDelPermStatement>(
                    groupName, dbName, std::move(perms));
        }
        throw DbException(StatusCode::SYNTAX_ERROR,
                          "ALTER GROUP: неизвестное действие");
    }

    if (target == "DATABASE") {
        consume();
        const std::string dbName =
            expect(TokenType::IDENTIFIER, "Ожидалось имя БД").value;
        validateIdentifier(dbName, "ALTER DATABASE");

        if (match(TokenType::KEYWORD, "ADD")) {
            if (match(TokenType::KEYWORD, "GROUP")) {
                const std::string gName =
                    expect(TokenType::IDENTIFIER, "Ожидалось имя группы").value;
                validateIdentifier(gName, "ALTER DATABASE ADD GROUP");
                match(TokenType::SEMICOLON);
                return std::make_unique<AlterDbAddGroupStatement>(dbName, gName);
            }
        }
        throw DbException(StatusCode::SYNTAX_ERROR,
                          "ALTER DATABASE: неизвестное действие");
    }

    throw DbException(StatusCode::SYNTAX_ERROR,
                      "Ожидалось USER, GROUP или DATABASE после ALTER, получено: '" +
                      peek().value + "'");
}

std::vector<std::string> SQLParser::parseIdentifierList() {
    expect(TokenType::LPAREN, "Ожидалось '(' перед списком");

    std::vector<std::string> list;
    do {
        const std::string id =
            expect(TokenType::IDENTIFIER, "Ожидался идентификатор в списке").value;
        validateIdentifier(id, "identifier list");
        list.push_back(id);
    } while (match(TokenType::COMMA));

    expect(TokenType::RPAREN, "Ожидалось ')' после списка");
    return list;
}

std::unique_ptr<ASTNode> SQLParser::parseExpression() {
    auto node = parseAndExpr();
    while (match(TokenType::KEYWORD, "OR")) {
        auto right = parseAndExpr();
        node = std::make_unique<LogicalNode>("OR", std::move(node), std::move(right));
    }
    return node;
}

std::unique_ptr<ASTNode> SQLParser::parseAndExpr() {
    auto node = parseNotExpr();
    while (match(TokenType::KEYWORD, "AND")) {
        auto right = parseNotExpr();
        node = std::make_unique<LogicalNode>("AND", std::move(node), std::move(right));
    }
    return node;
}

std::unique_ptr<ASTNode> SQLParser::parseNotExpr() {
    if (match(TokenType::KEYWORD, "NOT")) {
        auto child = parseNotExpr();
        return std::make_unique<NotNode>(std::move(child));
    }
    return parsePrimary();
}

std::unique_ptr<ASTNode> SQLParser::parsePrimary() {

    if (match(TokenType::LPAREN)) {
        auto inner = parseExpression();
        expect(TokenType::RPAREN, "Ожидалась закрывающая скобка ')'");
        return inner;
    }

    const Token lhsTok = consume();
    const std::string lhsName = lhsTok.value;

    if (match(TokenType::KEYWORD, "BETWEEN")) {
        const Token lowTok = consume();

        Value low = parseLiteral(lowTok.value);

        expect(TokenType::KEYWORD, "Ожидалось AND в конструкции BETWEEN ... AND ...");

        const Token highTok = consume();
        Value high = parseLiteral(highTok.value);

        return std::make_unique<BetweenNode>(lhsName, std::move(low), std::move(high));
    }

    if (match(TokenType::KEYWORD, "LIKE")) {
        const Token patTok =
            expect(TokenType::STRING_LITERAL,
                   "Ожидалась строка-регулярное выражение после LIKE");
        Value pattern = parseLiteral(patTok.value);
        return std::make_unique<ComparisonNode>(lhsName, "LIKE", std::move(pattern));
    }

    const Token opTok =
        expect(TokenType::OPERATOR, "Ожидался оператор сравнения (==, !=, <, >, <=, >=)");

    const Token rhsTok = consume();
    Value rhs = parseLiteral(rhsTok.value);

    return std::make_unique<ComparisonNode>(lhsName, opTok.value, std::move(rhs));
}

Value SQLParser::parseValue() {
    return parseLiteral(consume().value);
}

int SQLParser::findColumnIndex(const TableHeader& header, const std::string& colName) const {
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (std::string(header.columns[i].name) == colName)
            return static_cast<int>(i);
    }
    return -1;
}

bool SQLParser::applyConstraints(Row& row, const TableHeader& header,
                                 OutputCallback cb) const {
    for (uint32_t i = 0; i < header.column_count; ++i) {
        if (!row[i].is_null) continue;

        if (header.columns[i].has_default) {
            row[i] = parseLiteral(header.columns[i].default_val);
        } else if (header.columns[i].is_not_null || header.columns[i].is_indexed) {
            cb("[Ошибка] Нарушение NOT NULL: колонка '" +
               std::string(header.columns[i].name) + "' не может быть NULL\n");
            return false;
        }
    }
    return true;
}

bool SQLParser::prepareAndValidateRow(Row& outRow,
                                      const TableHeader& header,
                                      const std::vector<std::string>& targetCols,
                                      const std::vector<std::string>& rawValues,
                                      OutputCallback cb) {
    if (targetCols.empty()) {
        if (rawValues.size() > header.column_count) {
            cb("[Ошибка] Передано больше значений (" +
               std::to_string(rawValues.size()) +
               "), чем колонок в таблице (" +
               std::to_string(header.column_count) + ")\n");
            return false;
        }
        for (std::size_t i = 0; i < rawValues.size(); ++i)
            outRow[i] = parseLiteral(rawValues[i]);
    } else {
        if (targetCols.size() != rawValues.size()) {
            cb("[Ошибка] Количество колонок (" +
               std::to_string(targetCols.size()) +
               ") не совпадает с количеством значений (" +
               std::to_string(rawValues.size()) + ")\n");
            return false;
        }
        for (std::size_t i = 0; i < targetCols.size(); ++i) {
            const int idx = findColumnIndex(header, targetCols[i]);
            if (idx == -1) {
                cb("[Ошибка] Неизвестная колонка '" + targetCols[i] + "'\n");
                return false;
            }
            outRow[static_cast<std::size_t>(idx)] = parseLiteral(rawValues[i]);
        }
    }
    return applyConstraints(outRow, header, cb);
}