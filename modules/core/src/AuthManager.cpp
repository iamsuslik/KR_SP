#include "AuthManager.h"
#include "TableManager.h"
#include "ASTNode.h"
#include "DbException.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstring>

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
        while (valb >= 0) { out.push_back(lut[(val >> valb) & 0x3F]); valb -= 6; }
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
        if (valb >= 0) { out.push_back(static_cast<char>((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

std::string AuthManager::extractJsonValue(const std::string& json,
                                           const std::string& key) {
    if (json.empty()) return "";
    std::string sk = "\"" + key + "\":";
    size_t p = json.find(sk);
    if (p == std::string::npos) return "";
    p += sk.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '"')) ++p;
    size_t e = p;
    while (e < json.size() && json[e] != '"' && json[e] != ',' && json[e] != '}') ++e;
    return json.substr(p, e - p);
}

std::string AuthManager::createToken(const std::string& username) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string hdr = base64_encode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    std::string pay = base64_encode(
        "{\"user\":\"" + username + "\",\"iat\":" + std::to_string(now) + "}");
    std::string sig = hashPassword(hdr + "." + pay, "jwt_signature_salt");
    return hdr + "." + pay + "." + sig;
}

std::string AuthManager::verifyToken(const std::string& token) {
    size_t fd = token.find('.');
    size_t ld = token.rfind('.');
    if (fd == std::string::npos || ld == std::string::npos || fd == ld)
        throw DbException(StatusCode::AUTH_FAILED, "Некорректный формат токена");

    std::string hp  = token.substr(0, ld);
    std::string sig = token.substr(ld + 1);
    if (sig != hashPassword(hp, "jwt_signature_salt"))
        throw DbException(StatusCode::AUTH_FAILED, "Подпись токена не совпадает");

    std::string payload = base64_decode(token.substr(fd + 1, ld - fd - 1));
    std::string username = extractJsonValue(payload, "user");
    if (username.empty())
        throw DbException(StatusCode::AUTH_FAILED, "Повреждён payload токена");
    return username;
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
        c[0].is_indexed = true;
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
        c[0].is_indexed = true;
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

    std::string db_hash, db_salt;
    auto cb = [&](const std::string& row) {
        if (row.find('{') != std::string::npos) {
            db_hash = extractJsonValue(row, "pass_hash");
            db_salt = extractJsonValue(row, "salt");
        }
    };
    auto r = TableManager::executeSelect(path_res.path, cond.get(),
                                {"pass_hash", "salt"}, {}, {}, cb);
    r.throw_if_error();

    if (db_hash.empty() || db_salt.empty()) return false;
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

    bool granted = false;
    auto check_cb = [&](const std::string& r) {
        if (r.find('{') != std::string::npos) granted = true;
    };

    {
        auto c = std::make_unique<LogicalNode>("AND",
            std::make_unique<ComparisonNode>("grantee", "==", Value(std::string("PUBLIC"))),
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                std::make_unique<ComparisonNode>("action",  "==", Value(action))));
        auto r = TableManager::executeSelect(perm_path.path, c.get(), {"grantee"}, {}, {}, check_cb);
        r.throw_if_error();
        
        if (granted) return true;
    }

    {
        auto c = std::make_unique<LogicalNode>("AND",
            std::make_unique<ComparisonNode>("grantee",  "==", Value(username)),
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("is_group", "==", Value(0)),
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                    std::make_unique<ComparisonNode>("action",  "==", Value(action)))));
        TableManager::executeSelect(perm_path.path, c.get(), {"grantee"}, {}, {}, check_cb);
        if (granted) return true;
    }

    std::vector<std::string> groups;
    {
        auto gc = std::make_unique<ComparisonNode>("username", "==", Value(username));
        TableManager::executeSelect(ug_path.path, gc.get(), {"group_name"}, {}, {},
            [&](const std::string& r) {
                std::string g = extractJsonValue(r, "group_name");
                if (!g.empty()) groups.push_back(g);
            });
    }

    for (const auto& grp : groups) {
        auto c = std::make_unique<LogicalNode>("AND",
            std::make_unique<ComparisonNode>("grantee",  "==", Value(grp)),
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("is_group", "==", Value(1)),
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                    std::make_unique<ComparisonNode>("action",  "==", Value(action)))));
        TableManager::executeSelect(perm_path.path, c.get(), {"grantee"}, {}, {}, check_cb);
        if (granted) return true;
    }

    return false;
}
