#ifndef STATEMENTS_H
#define STATEMENTS_H

#include "common.h"
#include "ASTNode.h"
#include "HierarchyManager.h"
#include "TableManager.h"
#include "AsyncManager.h"
#include "AuthManager.h"
#include <fstream>
#include <sstream>

class Statement {
protected:

    Result requireAccess(int client_fd, const std::string& db, const std::string& action, HierarchyManager& hm) {
        if (!g_async_manager) return Result::Success();

        std::string current_user = g_async_manager->get_session_user(client_fd);
        if (current_user.empty()) return Result::Error(StatusCode::AUTH_FAILED, "Необходима авторизация (AUTH)");

        if (!AuthManager::checkAccess(current_user, db, action, hm)) {
            return Result::Error(StatusCode::PERMISSION_DENIED, "Отказ в доступе: требуется право " + action + " для " + db);
        }
        return Result::Success();
    }

public:
    virtual ~Statement() = default;
    virtual Result execute(HierarchyManager&hm,
                           int client_fd,
                           SQLParser::OutputCallback cb) = 0;
};

class SelectStatement : public Statement {
    std::string table_;
    std::vector<std::string> cols_;
    std::unique_ptr<ASTNode> where_;
    std::vector<AggregateRequest> aggs_;
    std::map<std::string, std::string> aliases_;

public:
    SelectStatement(std::string t,
                    std::vector<std::string> c,
                    std::unique_ptr<ASTNode> w,
                    std::vector<AggregateRequest>a  = {},
                    std::map<std::string, std::string> al = {})
        : table_(std::move(t)), cols_(std::move(c)), where_(std::move(w)),
          aggs_(std::move(a)), aliases_(std::move(al)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        std::string db_part = table_;
        auto dot = table_.find('.');
        if (dot != std::string::npos) db_part = table_.substr(0, dot);
        else if (g_async_manager) db_part = g_async_manager->get_session_db(fd);
        auto auth = requireAccess(fd, db_part, "READ", hm);
        if (!auth.isOk()) return auth;

        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;
        return TableManager::executeSelect(
            res.path, where_.get(), cols_, aliases_, aggs_, cb);
    }
};

class InsertStatement : public Statement {
    std::string table_;
    std::vector<std::string> targetCols_;
    std::vector<std::string> rawValues_;

public:
    InsertStatement(std::string t,
                    std::vector<std::string> c,
                    std::vector<std::string> v)
        : table_(std::move(t)), targetCols_(std::move(c)), rawValues_(std::move(v)) {}

    Result execute(HierarchyManager& hm, int fd, SQLParser::OutputCallback cb) override {

        std::string db_part = table_;
        auto dot = table_.find('.');
        if (dot != std::string::npos) {
            db_part = table_.substr(0, dot);
        } else if (g_async_manager) {
            db_part = g_async_manager->get_session_db(fd);
        }

        auto auth = requireAccess(fd, db_part, "WRITE", hm);
        if (!auth.isOk()) return auth;

        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;

        TableHeader header;

        Result readRes = Pager(res.path).read_page(0, &header);
        if (!readRes.isOk()) return readRes;

        Row finalRow(header.column_count, Value());
        SQLParser parser;

        if (parser.prepareAndValidateRow(finalRow, header, targetCols_, rawValues_, cb)) {
            Result insRes = TableManager::insertRow(res.path, finalRow);
            if (insRes.isOk()) cb("[Success] 1 row inserted.\n");
            return insRes;
        }

        return Result::Error(StatusCode::INVALID_VALUE, "Ошибка валидации строки при вставке");
    }
};

class InsertSelectStatement : public Statement {
    std::string table_;
    std::vector<std::string> targetCols_;
    std::unique_ptr<Statement> selectQuery_;

public:
    InsertSelectStatement(std::string t, std::vector<std::string> c, std::unique_ptr<Statement> s)
        : table_(std::move(t)), targetCols_(std::move(c)), selectQuery_(std::move(s)) {}

    Result execute(HierarchyManager& hm, int fd, SQLParser::OutputCallback cb) override {
        auto authRes = requireAccess(fd, table_, "WRITE", hm);
        if (!authRes.isOk()) return authRes;

        auto dstRes = hm.resolveTablePath(table_);
        if (!dstRes.isOk()) return dstRes;

        TableHeader dstHeader;
        Pager(dstRes.path).read_page(0, &dstHeader).throw_if_error();

        int inserted_count = 0;

        auto intercept_cb = [&](const std::string& row_json) {
            if (row_json == "[\n" || row_json == "\n]\n" || row_json == "[]\n") return;

            Row newRow(dstHeader.column_count, Value());
            SQLParser parser;

            std::vector<std::string> extracted_vals;
            for (uint32_t i = 0; i < dstHeader.column_count; ++i) {
                std::string key = "\"" + std::string(dstHeader.columns[i].name) + "\": ";
                size_t pos = row_json.find(key);
                if (pos != std::string::npos) {
                    pos += key.length();
                    size_t end = row_json.find_first_of(",}", pos);
                    std::string val = row_json.substr(pos, end - pos);
                    if (!val.empty() && val.front() == '"') val = val.substr(1, val.length() - 2);
                    extracted_vals.push_back(val);
                }
            }

            if (parser.prepareAndValidateRow(newRow, dstHeader, targetCols_, extracted_vals, cb)) {
                if (TableManager::insertRow(dstRes.path, newRow).isOk()) inserted_count++;
            }
        };

        Result selRes = selectQuery_->execute(hm, fd, intercept_cb);
        if (!selRes.isOk()) return selRes;

        cb("[Success] Успешно скопировано " + std::to_string(inserted_count) + " строк.\n");
        return Result::Success();
    }
};

class UpdateStatement : public Statement {
    std::string table_;
    std::vector<std::pair<std::string, Value>> assignments_;
    std::unique_ptr<ASTNode> where_;

public:
    UpdateStatement(std::string t,
                    std::vector<std::pair<std::string, Value>> a,
                    std::unique_ptr<ASTNode>  w)
        : table_(std::move(t)), assignments_(std::move(a)), where_(std::move(w)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        std::string db_part = table_;
        auto dot = table_.find('.');
        if (dot != std::string::npos) db_part = table_.substr(0, dot);
        else if (g_async_manager) db_part = g_async_manager->get_session_db(fd);

        auto auth = requireAccess(fd, db_part, "WRITE", hm);
        if (!auth.isOk()) return auth;

        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;

        auto r = TableManager::executeUpdate(res.path, where_.get(), assignments_);
        if (r.isOk()) cb(r.details + "\n");
        return r;
    }
};

class DeleteStatement : public Statement {
    std::string table_;
    std::unique_ptr<ASTNode> where_;

public:
    DeleteStatement(std::string t,
                    std::unique_ptr<ASTNode> w)
        : table_(std::move(t)), where_(std::move(w)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        std::string db_part = table_;
        auto dot = table_.find('.');
        if (dot != std::string::npos) db_part = table_.substr(0, dot);
        else if (g_async_manager) db_part = g_async_manager->get_session_db(fd);

        auto auth = requireAccess(fd, db_part, "WRITE", hm);
        if (!auth.isOk()) return auth;

        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;

        auto r = TableManager::executeDelete(res.path, where_.get());
        if (r.isOk()) cb(r.details + "\n");
        return r;
    }
};

class UseStatement : public Statement {
    std::string db_;

public:
    explicit UseStatement(std::string d) : db_(std::move(d)) {}

    Result execute(HierarchyManager& hm, int client_fd,
                   SQLParser::OutputCallback cb) override {
        Result res = hm.useDatabase(db_);
        if (res.isOk()) {
            if (g_async_manager)
                g_async_manager->set_session_db(client_fd, db_);
            cb(res.details + "\n");
        }
        return res;
    }
};

class CreateDbStatement : public Statement {
    std::string name_;
public:
    explicit CreateDbStatement(std::string n) : name_(std::move(n)) {}
    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto r = hm.createDatabase(name_);
        if (r.isOk()) cb(r.details + "\n");
        return r;
    }
};

class DropDbStatement : public Statement {
    std::string name_;
public:
    explicit DropDbStatement(std::string n) : name_(std::move(n)) {}
    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto r = hm.dropDatabase(name_);
        if (r.isOk()) cb(r.details + "\n");
        return r;
    }
};

class CreateTableStatement : public Statement {
    std::string             table_;
    std::vector<ColumnDef>  columns_;
public:
    CreateTableStatement(std::string t, std::vector<ColumnDef> c)
        : table_(std::move(t)), columns_(std::move(c)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath(table_);
        if (res.code == StatusCode::OK)
            return Result::Error(StatusCode::ALREADY_EXISTS,
                                 "Таблица '" + table_ + "' уже существует");
        if (res.code != StatusCode::NOT_FOUND)
            return res;

        auto r = TableManager::createTable(res.path, TableSchema(table_, columns_));
        if (r.isOk()) cb("Таблица '" + table_ + "' создана.\n");
        return r;
    }
};

class DropTableStatement : public Statement {
    std::string table_;
public:
    explicit DropTableStatement(std::string t) : table_(std::move(t)) {}
    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;
        auto r = TableManager::dropTable(res.path);
        if (r.isOk()) cb("Таблица '" + table_ + "' удалена.\n");
        return r;
    }
};


// class RevertStatement : public Statement {
//     std::string table_;
//     std::string timestamp_;
// public:
//     RevertStatement(std::string t, std::string ts) : table_(std::move(t)), timestamp_(std::move(ts)) {}

//     Result execute(HierarchyManager& hm, int fd, SQLParser::OutputCallback cb) override {
//         auto authRes = requireAccess(fd, table_, "ADMIN", hm);
//         if (!authRes.isOk()) return authRes;

//         auto res = hm.resolveTablePath(table_);
//         if (!res.isOk()) return res;

//         std::string undo_path = res.path + ".undo";
//         std::ifstream undo_file(undo_path, std::ios::binary);
        
//         if (!undo_file.is_open()) {
//             return Result::Error(StatusCode::NOT_FOUND, "Файл undo-лога для таблицы не найден. Откат невозможен.");
//         }
        
//         cb("[Temporal] Сканирование файла " + undo_path + "...\n");
//         cb("[Temporal] Поиск транзакций после " + timestamp_ + "...\n");
//         cb("[Temporal] Откат операций завершен успешно. Таблица восстановлена к состоянию на " + timestamp_ + ".\n");
        
//         return Result::Success();
//     }
// };

class GetTaskStatusStatement : public Statement {
    std::string guid_;
public:
    explicit GetTaskStatusStatement(std::string id) : guid_(std::move(id)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        if (!g_async_manager)
            return Result::Error(StatusCode::INTERNAL_ERROR, "AsyncManager недоступен");

        auto r = g_async_manager->fetch_result(guid_);
        switch (r.status) {
            case AsyncStatus::PENDING:
            case AsyncStatus::PROCESSING:
                cb("Processing...\n");
                return Result::Success();
            case AsyncStatus::COMPLETED:
                cb(r.data + "\n");
                return {r.db_code, r.data};
            case AsyncStatus::FAILED:
                cb("[Ошибка асинхронной задачи] " + r.data + "\n");
                return Result::Error(r.db_code, r.data);
        }
        return Result::Error(StatusCode::INTERNAL_ERROR, "Неизвестный статус задачи");
    }
};

class AuthStatement : public Statement {
    std::string user_, pass_;
public:
    AuthStatement(std::string u, std::string p) : user_(std::move(u)), pass_(std::move(p)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        if (AuthManager::authenticate(user_, pass_, hm)) {

            std::string token = AuthManager::createToken(user_);
            if (g_async_manager) g_async_manager->set_session_user(fd, user_);
            cb("{\"status\":\"success\", \"token\":\"" + token + "\"}\n");
            return Result::Success();
        }
        return Result::Error(StatusCode::AUTH_FAILED, "Неверный логин или пароль");
    }
};

class CreateUserStatement : public Statement {
    std::string user_, pass_;
public:
    CreateUserStatement(std::string u, std::string p) : user_(std::move(u)), pass_(std::move(p)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        std::string salt = AuthManager::generateRandomSalt();
        std::string hash = AuthManager::hashPassword(pass_, salt);
        
        Row userRow;
        userRow.push_back(Value(user_));
        userRow.push_back(Value(hash));
        userRow.push_back(Value(salt));

        auto res = hm.resolveTablePath("_system.users");
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "Системная БД не инициализирована");

        auto r = TableManager::insertRow(res.path, userRow);
        if (r.isOk()) cb("[Success] Пользователь '" + user_ + "' создан.\n");
        return r;
    }
};

class AlterDbAddGroupStatement : public Statement {
    std::string db_, group_;
public:
    AlterDbAddGroupStatement(std::string db, std::string g)
        : db_(std::move(db)), group_(std::move(g)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath("_system.groups");
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        Row row;
        row.push_back(Value(group_));
        row.push_back(Value(db_));
        
        auto r = TableManager::insertRow(res.path, row);
        if (r.isOk()) cb("[RBAC] Группа '" + group_ + "' создана в БД '" + db_ + "'.\n");
        return r;
    }
};

class AlterUserAddToGroupStatement : public Statement {
    std::string user_, db_, group_;
public:
    AlterUserAddToGroupStatement(std::string u, std::string db, std::string g)
        : user_(std::move(u)), db_(std::move(db)), group_(std::move(g)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath("_system.user_groups");
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        Row row;
        row.push_back(Value(user_));
        row.push_back(Value(group_));
        row.push_back(Value(db_));

        auto r = TableManager::insertRow(res.path, row);
        if (r.isOk()) cb("[RBAC] Пользователь '" + user_ + "' добавлен в группу '" + group_ + "' (БД: '" + db_ + "')\n");
        return r;
    }
};

class AlterUserAddPermStatement : public Statement {
    std::string user_, db_;
    std::vector<std::string> perms_;
public:
    AlterUserAddPermStatement(std::string u, std::string db, std::vector<std::string> p)
        : user_(std::move(u)), db_(std::move(db)), perms_(std::move(p)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath("_system.permissions");
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        for (const auto& perm : perms_) {
            Row row;
            row.push_back(Value(user_));
            row.push_back(Value(0));
            row.push_back(Value(db_));
            row.push_back(Value(perm));
            TableManager::insertRow(res.path, row);
        }
        cb("[RBAC] Права для пользователя '" + user_ + "' в БД '" + db_ + "' обновлены.\n");
        return Result::Success();
    }
};

class AlterGroupAddPermStatement : public Statement {
    std::string group_, db_;
    std::vector<std::string> perms_;
public:
    AlterGroupAddPermStatement(std::string g, std::string db, std::vector<std::string> p)
        : group_(std::move(g)), db_(std::move(db)), perms_(std::move(p)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath("_system.permissions");
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        for (const auto& perm : perms_) {
            Row row;
            row.push_back(Value(group_));
            row.push_back(Value(1));
            row.push_back(Value(db_));
            row.push_back(Value(perm));
            TableManager::insertRow(res.path, row);
        }
        cb("[RBAC] Права группы '" + group_ + "' в БД '" + db_ + "' добавлены.\n");
        return Result::Success();
    }
};

class AlterGroupDelPermStatement : public Statement {
    std::string group_, db_;
    std::vector<std::string> perms_;
public:
    AlterGroupDelPermStatement(std::string g, std::string db, std::vector<std::string> p)
        : group_(std::move(g)), db_(std::move(db)), perms_(std::move(p)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath("_system.permissions");
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        for (const auto& perm : perms_) {
            auto c1 = std::make_unique<ComparisonNode>("grantee", "==", Value(group_));
            auto c2 = std::make_unique<ComparisonNode>("is_group", "==", Value(1));
            auto c3 = std::make_unique<ComparisonNode>("db_name", "==", Value(db_));
            auto c4 = std::make_unique<ComparisonNode>("action", "==", Value(perm));

            auto and1 = std::make_unique<LogicalNode>("AND", std::move(c1), std::move(c2));
            auto and2 = std::make_unique<LogicalNode>("AND", std::move(c3), std::move(c4));
            auto final_cond = std::make_unique<LogicalNode>("AND", std::move(and1), std::move(and2));

            TableManager::executeDelete(res.path, final_cond.get());
        }
        
        cb("[RBAC] Права группы '" + group_ + "' в БД '" + db_ + "' отозваны.\n");
        return Result::Success();
    }
};

class AuthJwtStatement : public Statement {
    std::string token_;
public:
    explicit AuthJwtStatement(std::string t) : token_(std::move(t)) {}
    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        try {
            std::string user = AuthManager::verifyToken(token_);
            cb("{\"status\":\"success\", \"user\":\"" + user + "\", \"msg\":\"JWT Auth OK\"}\n");
            return Result::Success();
        } catch (const std::exception& e) {
            return Result::Error(StatusCode::AUTH_FAILED, e.what());
        }
    }
};

#endif  // STATEMENTS_H