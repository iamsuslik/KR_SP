#include "AuthManager.h"
#include "TableManager.h"
#include "ASTNode.h"
#include "DbException.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <iostream>

constexpr const char* AuthManager::SERVER_SECRET;

std::string AuthManager::hashPassword(const std::string& password, const std::string& salt) {
    std::string data = salt + ":" + password + ":" + SERVER_SECRET;
    uint64_t h1 = 0x123456789ABCDEF0ULL;
    uint64_t h2 = 0xFEDCBA9876543210ULL;

    for (char c : data) {
        h1 = (h1 ^ static_cast<uint64_t>(c)) * 0xBF58476D1CE4E5B9ULL;
        h2 = (h2 ^ static_cast<uint64_t>(c)) * 0x94D049BB133111EBULL;
    }
    std::stringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << h1 
       << std::setw(16) << std::setfill('0') << h2;
    return ss.str();
}

std::string AuthManager::generateRandomSalt(size_t len) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(alphabet) - 2);
    std::string salt;
    for(size_t i = 0; i < len; ++i) salt += alphabet[dis(gen)];
    return salt;
}

std::string AuthManager::base64_encode(const std::string& in) {
    static const char* lookup = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(lookup[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(lookup[((val << 8) >> (valb + 8)) & 0x3F]);
    return out;
}

std::string AuthManager::base64_decode(const std::string& in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string AuthManager::extractJsonValue(const std::string& json, const std::string& key) {
    if (json.empty()) return "";
    std::string search_key = "\"" + key + "\":";
    size_t pos = json.find(search_key);
    if (pos == std::string::npos) return "";
    
    pos += search_key.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '"')) pos++;
    
    size_t end_pos = pos;
    while (end_pos < json.length() && json[end_pos] != '"' && json[end_pos] != ',' && json[end_pos] != '}') end_pos++;
    
    return json.substr(pos, end_pos - pos);
}

std::string AuthManager::createToken(const std::string& username) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    
    std::string header = base64_encode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    std::string payload = base64_encode("{\"user\":\"" + username + "\",\"iat\":" + std::to_string(now) + "}");
    std::string signature = hashPassword(header + "." + payload, "jwt_signature_salt");
    
    return header + "." + payload + "." + signature;
}

std::string AuthManager::verifyToken(const std::string& token) {
    size_t first_dot = token.find('.');
    size_t last_dot = token.rfind('.');
    if (first_dot == std::string::npos || last_dot == std::string::npos || first_dot == last_dot) {
        throw DbException(StatusCode::AUTH_FAILED, "Некорректный формат токена");
    }

    std::string header_payload = token.substr(0, last_dot);
    std::string provided_sig = token.substr(last_dot + 1);
    std::string expected_sig = hashPassword(header_payload, "jwt_signature_salt");

    if (provided_sig != expected_sig) {
        throw DbException(StatusCode::AUTH_FAILED, "Подпись токена не совпадает (токен подделан)");
    }

    std::string payload_encoded = token.substr(first_dot + 1, last_dot - first_dot - 1);
    std::string payload_decoded = base64_decode(payload_encoded);
    
    std::string username = extractJsonValue(payload_decoded, "user");
    if (username.empty()) {
        throw DbException(StatusCode::AUTH_FAILED, "Поврежден payload токена (нет пользователя)");
    }
    
    return username;
}

bool AuthManager::authenticate(const std::string& username, const std::string& password, HierarchyManager& hm) {
    Result path_res = hm.resolveTablePath("_system.users");
    if (!path_res.isOk()) return false;

    std::unique_ptr<ASTNode> cond = std::make_unique<ComparisonNode>("login", "==", Value(username));

    std::string db_hash = "";
    std::string db_salt = "";

    auto callback = [&](const std::string& json_row) {
        if (json_row.find("{") != std::string::npos) {
            db_hash = extractJsonValue(json_row, "pass_hash");
            db_salt = extractJsonValue(json_row, "salt");
        }
    };

    TableManager::executeSelect(path_res.path, cond.get(), {"pass_hash", "salt"}, {}, {}, callback);

    if (db_hash.empty() || db_salt.empty()) return false;

    std::string calc_hash = hashPassword(password, db_salt);
    return calc_hash == db_hash;
}

bool AuthManager::checkAccess(const std::string& username, const std::string& resource, const std::string& action, HierarchyManager& hm) {
    if (username == "admin") return true;
    if (resource == "_system") return false;

    Result perm_path = hm.resolveTablePath("_system.permissions");
    Result ug_path = hm.resolveTablePath("_system.user_groups");
    if (!perm_path.isOk() || !ug_path.isOk()) return false;

    bool access_granted = false;
    auto check_cb = [&](const std::string& row) {
        if (row.find("{") != std::string::npos) access_granted = true;
    };

    auto d_cond = std::make_unique<LogicalNode>("AND",
        std::make_unique<ComparisonNode>("grantee", "==", Value("PUBLIC")),
        std::make_unique<LogicalNode>("AND",
            std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
            std::make_unique<ComparisonNode>("action", "==", Value(action))
        )
    );
    TableManager::executeSelect(perm_path.path, d_cond.get(), {"grantee"}, {}, {}, check_cb);
    if (access_granted) return true;

    auto u_cond = std::make_unique<LogicalNode>("AND",
        std::make_unique<ComparisonNode>("grantee", "==", Value(username)),
        std::make_unique<LogicalNode>("AND",
            std::make_unique<ComparisonNode>("is_group", "==", Value(0)),
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                std::make_unique<ComparisonNode>("action", "==", Value(action))
            )
        )
    );
    TableManager::executeSelect(perm_path.path, u_cond.get(), {"grantee"}, {}, {}, check_cb);
    if (access_granted) return true;

    std::vector<std::string> user_groups;
    auto g_find = std::make_unique<ComparisonNode>("username", "==", Value(username));
    TableManager::executeSelect(ug_path.path, g_find.get(), {"group_name"}, {}, {}, [&](const std::string& r) {
        std::string g = extractJsonValue(r, "group_name");
        if (!g.empty()) user_groups.push_back(g);
    });

    for (const auto& group : user_groups) {
        auto g_perm = std::make_unique<LogicalNode>("AND",
            std::make_unique<ComparisonNode>("grantee", "==", Value(group)),
            std::make_unique<LogicalNode>("AND",
                std::make_unique<ComparisonNode>("is_group", "==", Value(1)), // 1 = Группа
                std::make_unique<LogicalNode>("AND",
                    std::make_unique<ComparisonNode>("db_name", "==", Value(resource)),
                    std::make_unique<ComparisonNode>("action", "==", Value(action))
                )
            )
        );
        TableManager::executeSelect(perm_path.path, g_perm.get(), {"grantee"}, {}, {}, check_cb);
        if (access_granted) return true;
    }

    return false;
}

void AuthManager::initSystem(HierarchyManager& hm) {
    if (!hm.resolveTablePath("_system.users").isOk()) {
        hm.createDatabase("_system");

        std::vector<ColumnDef> user_cols = {
            ColumnDef("login", DataType::STR),
            ColumnDef("pass_hash", DataType::STR),
            ColumnDef("salt", DataType::STR)
        };
        user_cols[0].is_indexed = true; 
        TableManager::createTable(hm.resolveTablePath("_system.users").path, TableSchema("users", user_cols));

        std::vector<ColumnDef> grp_cols = {
            ColumnDef("group_name", DataType::STR),
            ColumnDef("db_name", DataType::STR)
        };
        grp_cols[0].is_indexed = true;
        TableManager::createTable(hm.resolveTablePath("_system.groups").path, TableSchema("groups", grp_cols));

        std::vector<ColumnDef> ug_cols = {
            ColumnDef("username", DataType::STR),
            ColumnDef("group_name", DataType::STR),
            ColumnDef("db_name", DataType::STR)
        };
        ug_cols[0].is_indexed = true;
        TableManager::createTable(hm.resolveTablePath("_system.user_groups").path, TableSchema("user_groups", ug_cols));

        std::vector<ColumnDef> perm_cols = {
            ColumnDef("grantee", DataType::STR),   
            ColumnDef("is_group", DataType::INT),  
            ColumnDef("db_name", DataType::STR),   
            ColumnDef("action", DataType::STR)     
        };
        perm_cols[0].is_indexed = true;
        TableManager::createTable(hm.resolveTablePath("_system.permissions").path, TableSchema("permissions", perm_cols));

        std::string salt = generateRandomSalt();
        Row admin_row;
        admin_row.push_back(Value("admin"));
        admin_row.push_back(Value(hashPassword("admin", salt)));
        admin_row.push_back(Value(salt));
        TableManager::insertRow(hm.resolveTablePath("_system.users").path, admin_row);
    }
}