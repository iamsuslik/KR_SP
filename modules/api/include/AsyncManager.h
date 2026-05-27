#ifndef ASYNC_MANAGER_H
#define ASYNC_MANAGER_H

#include "common.h"
#include <string>

struct AsyncResult {
    AsyncStatus status;
    std::string data;
    StatusCode db_code;
    std::string completion_time;
};

class AsyncManager {
private:
    SharedMemoryLayout* shm = nullptr; 
    size_t total_size = 0;

    std::string generate_guid_v4();
    SharedTaskSlot* find_free_slot();

public:
    AsyncManager();
    ~AsyncManager();

    // Задачи (GUID v4)
    std::string register_task();
    void update_task(const std::string& guid, AsyncStatus status, StatusCode code, const std::string& result_json);
    AsyncResult fetch_result(const std::string& guid);

    // Сессии (IPC / fork)
    void set_session_db(int fd, const std::string& db_name);
    std::string get_session_db(int fd);
    void close_session(int fd);

    AsyncManager(const AsyncManager&) = delete;
    AsyncManager& operator=(const AsyncManager&) = delete;
};

extern AsyncManager* g_async_manager;

#endif