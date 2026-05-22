#ifndef TABLE_LOCK_MANAGER_H
#define TABLE_LOCK_MANAGER_H

#include <string>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

class TableLockManager {
private:
    std::unordered_map<std::string, std::shared_mutex> locks;
    std::mutex map_mtx;

public:
    std::shared_mutex& get_lock(const std::string& path) {
        std::lock_guard<std::mutex> lock(map_mtx);
        return locks[path];
    }
};

inline TableLockManager g_lock_manager;

#endif
