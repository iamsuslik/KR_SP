#ifndef STATEMENTS_H
#define STATEMENTS_H

#include "common.h"
#include "ASTNode.h"
#include "HierarchyManager.h"
#include "TableManager.h"
#include "AsyncManager.h"

class Statement {
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

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;

        TableHeader header;
        Pager(res.path).read_page(0, &header).throw_if_error();

        Row finalRow(header.column_count, Value());
        SQLParser parser;
        if (parser.prepareAndValidateRow(finalRow, header, targetCols_, rawValues_, cb))
            return TableManager::insertRow(res.path, finalRow);

        return Result::Error(StatusCode::INVALID_VALUE, "Ошибка валидации строки при вставке");
    }
};

class InsertSelectStatement : public Statement {
    std::string table_;
    std::vector<std::string> targetCols_;
    std::unique_ptr<Statement> selectQuery_;

public:
    InsertSelectStatement(std::string t,
                          std::vector<std::string> c,
                          std::unique_ptr<Statement> s)
        : table_(std::move(t)), targetCols_(std::move(c)),
          selectQuery_(std::move(s)) {}

    Result execute(HierarchyManager& hm, int fd, SQLParser::OutputCallback cb) override {
        auto dstRes = hm.resolveTablePath(table_);
        if (!dstRes.isOk()) return dstRes;

        TableHeader dstHeader;
        Pager(dstRes.path).read_page(0, &dstHeader).throw_if_error();

        (void)fd;
        return Result::Error(StatusCode::INTERNAL_ERROR,
                             "INSERT INTO ... SELECT не реализован: "
                             "требуется внутренний итератор строк");
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

// ============================================================================
//  Дополнение 1: REVERT table timestamp
//  (темпоральная персистентность — стейтмент-заглушка до реализации ядра)
// ============================================================================

class RevertStatement : public Statement {
    std::string table_;
    std::string timestamp_;
public:
    RevertStatement(std::string t, std::string ts)
        : table_(std::move(t)), timestamp_(std::move(ts)) {}

    Result execute(HierarchyManager& hm, int, SQLParser::OutputCallback cb) override {
        auto res = hm.resolveTablePath(table_);
        if (!res.isOk()) return res;
        // TODO: реализовать через WAL/undo-log
        return Result::Error(StatusCode::INTERNAL_ERROR,
                             "REVERT не реализован: требуется WAL/undo-log");
    }
};

// ============================================================================
//  Дополнение 6: GET <guid> — статус асинхронной задачи
// ============================================================================

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

// ============================================================================
//  Дополнение 9: AUTH, CREATE USER
// ============================================================================

class AuthStatement : public Statement {
    std::string user_, pass_;
public:
    AuthStatement(std::string u, std::string p)
        : user_(std::move(u)), pass_(std::move(p)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        // TODO: проверка JWT / bcrypt-hash
        cb("[Auth] Аутентификация пользователя '" + user_ + "'...\n");
        return Result::Success();
    }
};

class CreateUserStatement : public Statement {
    std::string user_, pass_;
public:
    CreateUserStatement(std::string u, std::string p)
        : user_(std::move(u)), pass_(std::move(p)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        // TODO: хэширование пароля с солью, сохранение учётной записи
        cb("[Auth] Пользователь '" + user_ + "' создан.\n");
        return Result::Success();
    }
};

// ============================================================================
//  Дополнение 9: RBAC — ALTER USER / GROUP / DATABASE
// ============================================================================

class AlterUserAddToGroupStatement : public Statement {
    std::string user_, db_, group_;
public:
    AlterUserAddToGroupStatement(std::string u, std::string db, std::string g)
        : user_(std::move(u)), db_(std::move(db)), group_(std::move(g)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        cb("[RBAC] Пользователь '" + user_ + "' добавлен в группу '" +
           group_ + "' (БД: '" + db_ + "')\n");
        return Result::Success();
    }
};

class AlterUserAddPermStatement : public Statement {
    std::string              user_, db_;
    std::vector<std::string> perms_;
public:
    AlterUserAddPermStatement(std::string              u,
                              std::string              db,
                              std::vector<std::string> p)
        : user_(std::move(u)), db_(std::move(db)), perms_(std::move(p)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        cb("[RBAC] Права для пользователя '" + user_ + "' в БД '" + db_ + "' обновлены.\n");
        return Result::Success();
    }
};

class AlterGroupAddPermStatement : public Statement {
    std::string              group_, db_;
    std::vector<std::string> perms_;
public:
    AlterGroupAddPermStatement(std::string              g,
                               std::string              db,
                               std::vector<std::string> p)
        : group_(std::move(g)), db_(std::move(db)), perms_(std::move(p)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        cb("[RBAC] Права группы '" + group_ + "' в БД '" + db_ + "' добавлены.\n");
        return Result::Success();
    }
};

class AlterGroupDelPermStatement : public Statement {
    std::string              group_, db_;
    std::vector<std::string> perms_;
public:
    AlterGroupDelPermStatement(std::string              g,
                               std::string              db,
                               std::vector<std::string> p)
        : group_(std::move(g)), db_(std::move(db)), perms_(std::move(p)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        cb("[RBAC] Права группы '" + group_ + "' в БД '" + db_ + "' отозваны.\n");
        return Result::Success();
    }
};

class AlterDbAddGroupStatement : public Statement {
    std::string db_, group_;
public:
    AlterDbAddGroupStatement(std::string db, std::string g)
        : db_(std::move(db)), group_(std::move(g)) {}

    Result execute(HierarchyManager&, int, SQLParser::OutputCallback cb) override {
        cb("[RBAC] Группа '" + group_ + "' создана в БД '" + db_ + "'.\n");
        return Result::Success();
    }
};

#endif  // STATEMENTS_H