#ifndef STATEMENTS_H
#define STATEMENTS_H

#include "common.h"
#include "ASTNode.h"
#include "HierarchyManager.h"
#include "TableManager.h"
#include "AsyncManager.h"
#include "AuthManager.h"
#include "Pager.h"
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <cstring>

using json = nlohmann::json;

#ifndef OUTPUT_CALLBACK_DEFINED
#define OUTPUT_CALLBACK_DEFINED
using OutputCallback = std::function<void(const std::string&)>;
#endif

class Statement {
protected:

    Result requireAccess(const std::string& token, const std::string& resource,
                          const std::string& action, HierarchyManager& hm) {
        if (!g_async_manager) return Result::Success();

        if (token.empty()) {
            return Result::Error(StatusCode::AUTH_FAILED, 
                                 "Необходима авторизация (AUTH LOGIN ... PASSWORD ...)");
        }

        std::string user;
        try {
            user = AuthManager::verifyToken(token);
        } catch (const std::exception& e) {
            return Result::Error(StatusCode::AUTH_FAILED, 
                                 std::string("Ошибка верификации токена: ") + e.what());
        }

        if (!AuthManager::checkAccess(user, resource, action, hm)) {
            return Result::Error(StatusCode::PERMISSION_DENIED,
                                 "Отказ в доступе: требуется право '" + action + "' для '" + resource + "'");
        }
        return Result::Success();
    }

    static std::string db_of(const std::string& table, const std::string& token) {
        auto dot = table.find('.');
        if (dot != std::string::npos) return table.substr(0, dot);
        if (g_async_manager) return g_async_manager->get_session_db(token);
        return "";
    }

public:
    virtual ~Statement() = default;
    virtual Result execute(HierarchyManager& hm,
                           const std::string& token,
                           OutputCallback cb) = 0;
};

class UseStatement : public Statement {
    std::string db_;
public:
    explicit UseStatement(std::string d) : db_(std::move(d)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        Result r = hm.useDatabase(db_);
        if (r.isOk()) {
            if (g_async_manager) {
                g_async_manager->set_session_db(token, db_);
            }
            cb("[Success] " + r.details + "\n");
        } else {
            cb("[Error] " + r.details + "\n");
        }
        return r;
    }
};

class CreateDbStatement : public Statement {
    std::string name_;
public:
    explicit CreateDbStatement(std::string n) : name_(std::move(n)) {}
    
    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, name_, "CREATE", hm);
        if (!auth.isOk()) { 
            cb("[Error] " + auth.details + "\n"); 
            return auth; 
        }
        Result r = hm.createDatabase(name_);
        if (r.isOk()) cb("[Success] " + r.details + "\n");
        else          cb("[Error] " + r.details + "\n");
        return r;
    }
};

class DropDbStatement : public Statement {
    std::string name_;
public:
    explicit DropDbStatement(std::string n) : name_(std::move(n)) {}
    
    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, name_, "DROP", hm);
        if (!auth.isOk()) { 
            cb("[Error] " + auth.details + "\n"); 
            return auth; 
        }
        Result r = hm.dropDatabase(name_);
        if (r.isOk()) cb("[Success] " + r.details + "\n");
        else          cb("[Error] " + r.details + "\n");
        return r;
    }
};

class CreateTableStatement : public Statement {
    std::string            table_;
    std::vector<ColumnDef> cols_;
public:
    CreateTableStatement(std::string t, std::vector<ColumnDef> c)
        : table_(std::move(t)), cols_(std::move(c)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "CREATE", hm);
        if (!auth.isOk()) { 
            cb("[Error] " + auth.details + "\n"); 
            return auth; 
        }

        Result res = hm.resolveTablePath(table_, g_current_node_id);
        if (res.code == StatusCode::DATABASE_NOT_FOUND) {
            cb("[Error] " + res.details + "\n"); 
            return res;
        }
        if (res.isOk()) {
            cb("[Error] Таблица '" + table_ + "' уже существует.\n");
            return Result::Error(StatusCode::ALREADY_EXISTS, "Table already exists");
        }
        Result r = TableManager::createTable(res.path, TableSchema(table_, cols_));
        if (r.isOk()){ 
            cb("[Success] Таблица '" + table_ + "' создана.\n");
        }else{
            cb("[Error] " + r.details + "\n");
        }
        return r;
    }
};

class DropTableStatement : public Statement {
    std::string table_;
public:
    explicit DropTableStatement(std::string t) : table_(std::move(t)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "DROP", hm);
        if (!auth.isOk()) { 
            cb("[Error] " + auth.details + "\n"); 
            return auth; 
        }

        Result res = hm.resolveTablePath(table_, g_current_node_id);
        if (!res.isOk()) { 
            cb("[Error] " + res.details + "\n"); 
            return res; 
        }
        Result r = TableManager::dropTable(res.path);
        if (r.isOk()) cb("[Success] Таблица '" + table_ + "' удалена.\n");
        else          cb("[Error] " + r.details + "\n");
        return r;
    }
};

class InsertStatement : public Statement {
    std::string              table_;
    std::vector<std::string> target_cols_;
    std::vector<std::string> rawValues_;

public:
    InsertStatement(std::string t,
                    std::vector<std::string> tc,
                    std::vector<std::string> v) 
        : table_(std::move(t)), 
          target_cols_(std::move(tc)), 
          rawValues_(std::move(v)) {} 

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "WRITE", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        Result res = hm.resolveTablePath(table_, g_current_node_id);
        if (!res.isOk()) { cb("[Error] " + res.details + "\n"); return res; }

        TableHeader header;
        try { 
            Pager(res.path).read_page(0, &header).throw_if_error(); 
        }
        catch (const DbException& e) {
            cb("[Error] " + std::string(e.what()) + "\n");
            return Result::Error(e.code(), e.what());
        }

        Row row(header.column_count, Value());

        if (!target_cols_.empty()) {
            for (size_t i = 0; i < target_cols_.size() && i < rawValues_.size(); ++i) {
                int ci = find_col(header, target_cols_[i]);
                if (ci == -1) {
                    cb("[Error] Колонка '" + target_cols_[i] + "' не найдена.\n");
                    return Result::Error(StatusCode::COLUMN_NOT_FOUND, "Column not found");
                }
                row[ci] = SQLParser::parseLiteral(rawValues_[i]); 
            }
        } else {
            if (rawValues_.size() != header.column_count) {
                cb("[Error] Несоответствие числа столбцов. Ожидалось "
                   + std::to_string(header.column_count) + ".\n");
                return Result::Error(StatusCode::INVALID_VALUE, "Column count mismatch");
            }
            for (size_t i = 0; i < rawValues_.size(); ++i) {
                row[i] = SQLParser::parseLiteral(rawValues_[i]);
            }
        }

        if (!apply_constraints(row, header, cb))
            return Result::Error(StatusCode::NOT_NULL_VIOLATION, "Constraint violation");

        Result ins = TableManager::insertRow(res.path, row);
        if (ins.isOk()){ 
            cb("[Success] " + ins.details + "\n");
        } else {
            cb("[Error] " + ins.details + "\n");
        }
        return ins;
    }

private:
    static int find_col(const TableHeader& h, const std::string& name) {
        for (uint32_t i = 0; i < h.column_count; ++i)
            if (std::string(h.columns[i].name) == name) return static_cast<int>(i);
        return -1;
    }

    static bool apply_constraints(Row& row, const TableHeader& h, OutputCallback cb) {
        for (uint32_t i = 0; i < h.column_count; ++i) {
            if (!row[i].is_null) continue;
            if (h.columns[i].has_default) {
                row[i] = SQLParser::parseLiteral(h.columns[i].default_val);
            } else if (h.columns[i].is_not_null) {
                cb("[Error] Колонка '" + std::string(h.columns[i].name) + "' NOT_NULL.\n");
                return false;
            }
        }
        return true;
    }
};

class InsertSelectStatement : public Statement {
    std::string                table_;
    std::vector<std::string>   target_cols_;
    std::unique_ptr<Statement> select_query_;

public:
    InsertSelectStatement(std::string t,
                          std::vector<std::string> c,
                          std::unique_ptr<Statement> s)
        : table_(std::move(t)), target_cols_(std::move(c)),
          select_query_(std::move(s)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "WRITE", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        Result dst_res = hm.resolveTablePath(table_, g_current_node_id);
        if (!dst_res.isOk()) { cb("[Error] " + dst_res.details + "\n"); return dst_res; }

        TableHeader dst_header;
        try { Pager(dst_res.path).read_page(0, &dst_header).throw_if_error(); }
        catch (const DbException& e) {
            cb("[Error] " + std::string(e.what()) + "\n");
            return Result::Error(e.code(), e.what());
        }

        int inserted_count = 0;

        std::string select_buf;
        auto intercept_cb = [&](const std::string& chunk) { select_buf += chunk; };

        Result sel_res = select_query_->execute(hm, token, intercept_cb);
        if (!sel_res.isOk()) { cb("[Error] " + sel_res.details + "\n"); return sel_res; }

        std::vector<json> src_rows;
        try {
            json arr = json::parse(select_buf);
            if (arr.is_array())
                for (const auto& item : arr)
                    if (item.is_object()) src_rows.push_back(item);
        } catch (...) {
            cb("[Error] Не удалось разобрать результат SELECT.\n");
            return Result::Error(StatusCode::INTERNAL_ERROR, "SELECT output parse failed");
        }

        for (const auto& src_row : src_rows) {
            Row new_row(dst_header.column_count, Value());
            bool ok = true;

            if (!target_cols_.empty()) {
                for (size_t i = 0; i < target_cols_.size(); ++i) {
                    if (!src_row.contains(target_cols_[i])) continue;
                    int ci = find_col(dst_header, target_cols_[i]);
                    if (ci == -1) { ok = false; break; }
                    new_row[ci] = json_to_value(src_row[target_cols_[i]]);
                }
            } else {
                for (uint32_t i = 0; i < dst_header.column_count; ++i) {
                    std::string col_name(dst_header.columns[i].name);
                    if (src_row.contains(col_name))
                        new_row[i] = json_to_value(src_row[col_name]);
                }
            }

            if (!ok) continue;
            if (TableManager::insertRow(dst_res.path, new_row).isOk()) ++inserted_count;
        }

        cb("[Success] Скопировано " + std::to_string(inserted_count) + " строк.\n");
        return Result::Success();
    }

private:
    static int find_col(const TableHeader& h, const std::string& name) {
        for (uint32_t i = 0; i < h.column_count; ++i)
            if (std::string(h.columns[i].name) == name) return static_cast<int>(i);
        return -1;
    }

    static Value json_to_value(const json& j) {
        if (j.is_null())    return Value();
        if (j.is_number_integer()) return Value(j.get<int>());
        if (j.is_string())  return Value(j.get<std::string>());
        if (j.is_number())  return Value(static_cast<int>(j.get<double>()));
        return Value(j.dump());
    }
};

class SelectStatement : public Statement {
    std::string                        table_;
    std::vector<std::string>           cols_;
    std::unique_ptr<ASTNode>           where_;
    std::vector<AggregateRequest>      aggs_;
    std::map<std::string,std::string>  aliases_;
public:
    SelectStatement(std::string t, 
                    std::vector<std::string> c, 
                    std::unique_ptr<ASTNode> w,
                    std::vector<AggregateRequest> agg, 
                    std::map<std::string, std::string> a)
        : table_(std::move(t)), cols_(std::move(c)), where_(std::move(w)), 
          aggs_(std::move(agg)), aliases_(std::move(a)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "READ", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        Result res = hm.resolveTablePath(table_, g_current_node_id);
        if (!res.isOk()) { cb("[Error] " + res.details + "\n"); return res; }
        return TableManager::executeSelect(res.path, where_.get(),
                                           cols_, aliases_, aggs_, cb);
    }
};

class UpdateStatement : public Statement {
    std::string                                 table_;
    std::vector<std::pair<std::string,Value>>   asgn_;
    std::unique_ptr<ASTNode>                    where_;
public:
    UpdateStatement(std::string t,
                    std::vector<std::pair<std::string,Value>> a,
                    std::unique_ptr<ASTNode> w)
        : table_(std::move(t)), asgn_(std::move(a)), where_(std::move(w)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "WRITE", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        Result res = hm.resolveTablePath(table_, g_current_node_id);
        if (!res.isOk()) { cb("[Error] " + res.details + "\n"); return res; }

        Result r = TableManager::executeUpdate(res.path, where_.get(), asgn_);
        if (r.isOk()) cb("[Success] " + r.details + "\n");
        else          cb("[Error] "   + r.details + "\n");
        return r;
    }
};

class DeleteStatement : public Statement {
    std::string              table_;
    std::unique_ptr<ASTNode> where_;
public:
    DeleteStatement(std::string t, std::unique_ptr<ASTNode> w)
        : table_(std::move(t)), where_(std::move(w)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, db_of(table_, token), "WRITE", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        Result res = hm.resolveTablePath(table_, g_current_node_id);
        if (!res.isOk()) { cb("[Error] " + res.details + "\n"); return res; }

        Result r = TableManager::executeDelete(res.path, where_.get());
        if (r.isOk()) cb("[Success] " + r.details + "\n");
        else          cb("[Error] "   + r.details + "\n");
        return r;
    }
};

class AuthStatement : public Statement {
    std::string user_, pass_;
public:
    AuthStatement(std::string u, std::string p)
        : user_(std::move(u)), pass_(std::move(p)) {}

    Result execute(HierarchyManager& hm, const std::string&, OutputCallback cb) override {
        if (!AuthManager::authenticate(user_, pass_, hm)) {
            cb("[Error] Неверный логин или пароль.\n");
            return Result::Error(StatusCode::AUTH_FAILED, "Authentication failed");
        }
        std::string token = AuthManager::createToken(user_);

        json resp = {{"status", "success"}, {"token", token}};
        cb(resp.dump() + "\n");
        return Result::Success();
    }
};

class AuthJwtStatement : public Statement {
    std::string token_;
public:
    explicit AuthJwtStatement(std::string t) : token_(std::move(t)) {}

    Result execute(HierarchyManager&, const std::string&, OutputCallback cb) override {
        try {
            std::string user = AuthManager::verifyToken(token_);
            json resp = {{"status", "success"}, {"user", user}, {"msg", "JWT verified"}};
            cb(resp.dump() + "\n");
            return Result::Success();
        } catch (const DbException& e) {
            json resp = {{"status", "error"}, {"msg", e.what()}};
            cb(resp.dump() + "\n");
            return Result::Error(e.code(), e.what());
        }
    }
};

class GetTaskStatusStatement : public Statement {
    std::string guid_;
public:
    explicit GetTaskStatusStatement(std::string id) : guid_(std::move(id)) {}

    Result execute(HierarchyManager&, const std::string&, OutputCallback cb) override {
        if (!g_async_manager)
            return Result::Error(StatusCode::INTERNAL_ERROR, "AsyncManager недоступен");

        auto r = g_async_manager->fetch_result(guid_);

        if (r.status == AsyncStatus::PENDING || r.status == AsyncStatus::PROCESSING) {
            json resp = {{"status", "processing"}, {"guid", guid_}};
            cb(resp.dump() + "\n");
            return Result::Success();
        }
        if (r.status == AsyncStatus::COMPLETED) {
            cb(r.data + "\n");
            return {r.db_code, r.data};
        }
        json resp = {{"status", "error"}, {"msg", r.data}};
        cb(resp.dump() + "\n");
        return Result::Error(r.db_code, r.data);
    }
};

class CreateUserStatement : public Statement {
    std::string user_, pass_;
public:
    CreateUserStatement(std::string u, std::string p)
        : user_(std::move(u)), pass_(std::move(p)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, "_system", "ADMIN", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        auto res = hm.resolveTablePath("_system.users", g_current_node_id);
        if (!res.isOk()) {
            cb("[Error] Системная БД не инициализирована.\n"); return res;
        }

        std::string salt = AuthManager::generateRandomSalt();
        Row row;
        row.push_back(Value(user_));
        row.push_back(Value(AuthManager::hashPassword(pass_, salt)));
        row.push_back(Value(salt));

        Result r = TableManager::insertRow(res.path, row);
        if (r.isOk()) cb("[Success] Пользователь '" + user_ + "' создан.\n");
        else          cb("[Error] " + r.details + "\n");
        return r;
    }
};

class AlterDbAddGroupStatement : public Statement {
    std::string db_, group_;
public:
    AlterDbAddGroupStatement(std::string db, std::string g)
        : db_(std::move(db)), group_(std::move(g)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, "_system", "ADMIN", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        auto res = hm.resolveTablePath("_system.groups", g_current_node_id);
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        Row row; row.push_back(Value(group_)); row.push_back(Value(db_));
        Result r = TableManager::insertRow(res.path, row);
        if (r.isOk()) cb("[RBAC] Группа '" + group_ + "' создана для БД '" + db_ + "'.\n");
        return r;
    }
};

class AlterUserAddToGroupStatement : public Statement {
    std::string user_, db_, group_;
public:
    AlterUserAddToGroupStatement(std::string u, std::string db, std::string g)
        : user_(std::move(u)), db_(std::move(db)), group_(std::move(g)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, "_system", "ADMIN", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        auto res = hm.resolveTablePath("_system.user_groups", g_current_node_id);
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        Row row;
        row.push_back(Value(user_)); row.push_back(Value(group_)); row.push_back(Value(db_));
        Result r = TableManager::insertRow(res.path, row);
        if (r.isOk())
            cb("[RBAC] Пользователь '" + user_ + "' добавлен в группу '"
               + group_ + "' (БД: '" + db_ + "').\n");
        return r;
    }
};

class GrantPermStatement : public Statement {
    std::string              grantee_;
    bool                     is_group_;
    std::string              db_;
    std::vector<std::string> actions_;
public:
    GrantPermStatement(std::string g, bool is_grp, std::string db,
                        std::vector<std::string> a)
        : grantee_(std::move(g)), is_group_(is_grp),
          db_(std::move(db)), actions_(std::move(a)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, "_system", "ADMIN", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        auto res = hm.resolveTablePath("_system.permissions", g_current_node_id);
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        for (const auto& act : actions_) {
            Row row;
            row.push_back(Value(grantee_));
            row.push_back(Value(is_group_ ? 1 : 0));
            row.push_back(Value(db_));
            row.push_back(Value(act));
            Result r = TableManager::insertRow(res.path, row);
            if (!r.isOk()) return r;
        }
        cb("[RBAC] Права выданы '" + grantee_ + "' для БД '" + db_ + "'.\n");
        return Result::Success();
    }
};

class RevokePermStatement : public Statement {
    std::string              grantee_;
    bool                     is_group_;
    std::string              db_;
    std::vector<std::string> actions_;
public:
    RevokePermStatement(std::string g, bool is_grp, std::string db,
                         std::vector<std::string> a)
        : grantee_(std::move(g)), is_group_(is_grp),
          db_(std::move(db)), actions_(std::move(a)) {}

    Result execute(HierarchyManager& hm, const std::string& token, OutputCallback cb) override {
        auto auth = requireAccess(token, "_system", "ADMIN", hm);
        if (!auth.isOk()) { cb("[Error] " + auth.details + "\n"); return auth; }

        auto res = hm.resolveTablePath("_system.permissions", g_current_node_id);
        if (!res.isOk()) return Result::Error(StatusCode::INTERNAL_ERROR, "RBAC не инициализирован");

        for (const auto& act : actions_) {
            auto cond = std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("grantee",  "==", Value(grantee_)),
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("is_group", "==", Value(is_group_ ? 1 : 0)),
                    std::make_unique<LogicalNode>("AND",
                        std::make_unique<ComparisonNode>("db_name", "==", Value(db_)),
                        std::make_unique<ComparisonNode>("action",  "==", Value(act)))));
            Result r = TableManager::executeDelete(res.path, cond.get());
            if (!r.isOk()) return r;
        }
        cb("[RBAC] Права отозваны у '" + grantee_ + "' для БД '" + db_ + "'.\n");
        return Result::Success();
    }
};

class AlterUserAddPermStatement : public GrantPermStatement {
public:
    AlterUserAddPermStatement(std::string u, std::string db, std::vector<std::string> p)
        : GrantPermStatement(std::move(u), false, std::move(db), std::move(p)) {}
};

class AlterGroupAddPermStatement : public GrantPermStatement {
public:
    AlterGroupAddPermStatement(std::string g, std::string db, std::vector<std::string> p)
        : GrantPermStatement(std::move(g), true, std::move(db), std::move(p)) {}
};

class AlterGroupDelPermStatement : public RevokePermStatement {
public:
    AlterGroupDelPermStatement(std::string g, std::string db, std::vector<std::string> p)
        : RevokePermStatement(std::move(g), true, std::move(db), std::move(p)) {}
};


#endif // STATEMENTS_H
