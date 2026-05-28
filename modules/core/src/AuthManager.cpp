#include "AuthManager.h"
#include "TableManager.h"
#include "ASTNode.h"
#include "DbException.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>

using json = nlohmann::json;
 
constexpr const char* AuthManager::SERVER_SECRET;

std::string AuthManager::hashPassword(const std::string& password,
                                       const std::string& salt) {
    std::string data = salt + ":" + password + ":" + SERVER_SECRET;
    uint64_t h1 = 0x123456789ABCDEF0ULL;
    uint64_t h2 = 0xFEDCBA9876543210ULL;
    for (char c : data) {
        h1 = (h1 ^ static_cast<uint64_t>(c)) * 0xBF58476D1CE4E5B9ULL;
        h2 = (h2 ^ static_cast<uint64_t>(c)) * 0x94D049BB133111EBULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << h1
                   << std::setw(16) << std::setfill('0') << h2;
    return ss.str();
}

std::string AuthManager::generateRandomSalt(size_t len) {
    static const char alpha[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(sizeof(alpha) - 2));
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) s += alpha[dis(gen)];
    return s;
}

std::string AuthManager::base64_encode(const std::string& in) {
    static const char* lut =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { 
            out.push_back(lut[(val >> valb) & 0x3F]); 
            valb -= 6; 
        }
    }
    if (valb > -6) out.push_back(lut[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

std::string AuthManager::base64_decode(const std::string& in) {
    std::string out;
    std::vector<int> T(256, -1);
    const char* lut =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(lut[i])] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c]; valb += 6;
        if (valb >= 0) { 
            out.push_back(static_cast<char>((val >> valb) & 0xFF)); 
            valb -= 8; 
        }
    }
    return out;
}

std::vector<json> AuthManager::parseSelectOutput(const std::string& raw) {
    std::vector<json> rows;
    if (raw.empty()) return rows;
    try {
        json arr = json::parse(raw);
        if (arr.is_array()) {
            for (const auto& item : arr)
                if (item.is_object()) rows.push_back(item);
        }
    } catch (...) {
    }
    return rows;
}

std::string AuthManager::createToken(const std::string& username) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    json payload_obj = {
        {"user", username},
        {"iat",  static_cast<int64_t>(now)}
    };
    json header_obj = {{"alg", "HS256"}, {"typ", "JWT"}};
 
    std::string hdr = base64_encode(header_obj.dump());
    std::string pay = base64_encode(payload_obj.dump());
    std::string sig = hashPassword(hdr + "." + pay, "jwt_signature_salt");
    return hdr + "." + pay + "." + sig;
}

std::string AuthManager::verifyToken(const std::string& token) {
    size_t fd = token.find('.');
    size_t ld = token.rfind('.');
    if (fd == std::string::npos || ld == std::string::npos || fd == ld){
        throw DbException(StatusCode::AUTH_FAILED, "Некорректный формат токена");
    }

    std::string hp  = token.substr(0, ld);
    std::string sig = token.substr(ld + 1);
    if (sig != hashPassword(hp, "jwt_signature_salt"))
        throw DbException(StatusCode::AUTH_FAILED, "Подпись токена не совпадает");

    std::string payload_raw = base64_decode(token.substr(fd + 1, ld - fd - 1));
 
    try {
        json payload = json::parse(payload_raw);
        if (!payload.contains("user") || !payload["user"].is_string()){
            throw DbException(StatusCode::AUTH_FAILED, "Повреждён payload токена");
        }
        return payload["user"].get<std::string>();
    } catch (const DbException&) {
        throw;
    } catch (...) {
        throw DbException(StatusCode::AUTH_FAILED, "Ошибка разбора payload токена");
    }
}

void AuthManager::initSystem(HierarchyManager& hm) {
    static constexpr int SYS_FD = -1;

    if (hm.resolveTablePath("_system.users", SYS_FD).isOk()) return;

    hm.createDatabase("_system").throw_if_error(); 

    {
        std::vector<ColumnDef> c = {
            ColumnDef("login",     DataType::STR),
            ColumnDef("pass_hash", DataType::STR),
            ColumnDef("salt",      DataType::STR)
        };
        c[0].is_indexed = true;
        auto p = hm.resolveTablePath("_system.users", SYS_FD);
        TableManager::createTable(p.path, TableSchema("users", c)).throw_if_error();
    }

    {
        std::vector<ColumnDef> c = {
            ColumnDef("group_name", DataType::STR),
            ColumnDef("db_name",    DataType::STR)
        };
        c[0].is_indexed = true;
        auto p = hm.resolveTablePath("_system.groups", SYS_FD);
        TableManager::createTable(p.path, TableSchema("groups", c)).throw_if_error();
    }

    {
        std::vector<ColumnDef> c = {
            ColumnDef("username",   DataType::STR),
            ColumnDef("group_name", DataType::STR),
            ColumnDef("db_name",    DataType::STR)
        };
        c[0].is_indexed = false;
        auto p = hm.resolveTablePath("_system.user_groups", SYS_FD);
        TableManager::createTable(p.path, TableSchema("user_groups", c)).throw_if_error();
    }
    {
        std::vector<ColumnDef> c = {
            ColumnDef("grantee",  DataType::STR),
            ColumnDef("is_group", DataType::INT),
            ColumnDef("db_name",  DataType::STR),
            ColumnDef("action",   DataType::STR)
        };
        c[0].is_indexed = false;
        auto p = hm.resolveTablePath("_system.permissions", SYS_FD);
        TableManager::createTable(p.path, TableSchema("permissions", c)).throw_if_error();
    }

    std::string salt = generateRandomSalt();
    Row admin_row;
    admin_row.push_back(Value(std::string("admin")));
    admin_row.push_back(Value(hashPassword("admin", salt)));
    admin_row.push_back(Value(salt));
    auto up = hm.resolveTablePath("_system.users", SYS_FD);
    TableManager::insertRow(up.path, admin_row).throw_if_error();
}

bool AuthManager::authenticate(const std::string& username,
                                const std::string& password,
                                HierarchyManager&  hm) {
    static constexpr int SYS_FD = -1;
    Result path_res = hm.resolveTablePath("_system.users", SYS_FD);
    if (!path_res.isOk()) return false;

    auto cond = std::make_unique<ComparisonNode>("login", "==", Value(username));

    std::string raw_output;
    auto cb = [&](const std::string& chunk) { raw_output += chunk; };
 
    TableManager::executeSelect(
        path_res.path, cond.get(), {"pass_hash", "salt"}, {}, {}, cb).throw_if_error();

    auto rows = parseSelectOutput(raw_output);
    if (rows.empty()) return false;
 
    const json& row = rows[0];
    if (!row.contains("pass_hash") || !row.contains("salt")) return false;
 
    std::string db_hash = row["pass_hash"].get<std::string>();
    std::string db_salt = row["salt"].get<std::string>();
 
    return hashPassword(password, db_salt) == db_hash;
}


bool AuthManager::checkAccess(const std::string& username,
                               const std::string& resource,
                               const std::string& action,
                               HierarchyManager&  hm) {
    if (username == "admin") return true;
    if (resource == "_system") return false;

    static constexpr int SYS_FD = -1;
    Result perm_path = hm.resolveTablePath("_system.permissions", SYS_FD);
    Result ug_path   = hm.resolveTablePath("_system.user_groups",  SYS_FD);
    if (!perm_path.isOk() || !ug_path.isOk()) return false;

    auto has_match = [&](const std::string& path,
                          std::unique_ptr<ASTNode> cond) -> bool {
        std::string raw;
        auto cb = [&](const std::string& s) { raw += s; };
        TableManager::executeSelect(path, cond.get(), {}, {}, {}, cb).throw_if_error();
        return !parseSelectOutput(raw).empty();
    };

    if (has_match(perm_path.path,
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("grantee",  "==", Value(std::string("PUBLIC"))),
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                    std::make_unique<ComparisonNode>("action",  "==", Value(action))))))
        return true;

    if (has_match(perm_path.path,
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("grantee",  "==", Value(username)),
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("is_group", "==", Value(0)),
                    std::make_unique<LogicalNode>("AND",
                        std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                        std::make_unique<ComparisonNode>("action",  "==", Value(action)))))))
        return true;

    std::vector<std::string> groups;
    {
        std::string raw;
        auto cb = [&](const std::string& s) { raw += s; };
        auto gc = std::make_unique<ComparisonNode>("username", "==", Value(username));
        TableManager::executeSelect(ug_path.path, gc.get(), {"group_name"}, {}, {}, cb).throw_if_error();
 
        for (const auto& row : parseSelectOutput(raw)) {
            if (row.contains("group_name") && row["group_name"].is_string())
                groups.push_back(row["group_name"].get<std::string>());
        }
    }

    for (const auto& grp : groups) {
        if (has_match(perm_path.path,
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("grantee",  "==", Value(grp)),
                    std::make_unique<LogicalNode>("AND",
                        std::make_unique<ComparisonNode>("is_group", "==", Value(1)),
                        std::make_unique<LogicalNode>("AND",
                            std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                            std::make_unique<ComparisonNode>("action",  "==", Value(action)))))))
            return true;
    }
 
    return false;
}
