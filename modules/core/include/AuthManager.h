#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <string>
#include <vector>
#include "common.h"
#include "HierarchyManager.h"

class AuthManager {
private:
    static constexpr const char* SERVER_SECRET = "industrial_db_secret_key_2024_pro";

    static std::string base64_encode(const std::string& in);
    static std::string base64_decode(const std::string& in);

    static std::string extractJsonValue(const std::string& json, const std::string& key);

public:

    static std::string generateRandomSalt(size_t len = 16);

    static std::string hashPassword(const std::string& password, const std::string& salt);

    static std::string createToken(const std::string& username);

    static std::string verifyToken(const std::string& token);

    static void initSystem(HierarchyManager& hm);

    static bool authenticate(const std::string& username, const std::string& password, HierarchyManager& hm);

    static bool checkAccess(const std::string& username, const std::string& resource, const std::string& action, HierarchyManager& hm);
};

#endif // AUTH_MANAGER_H