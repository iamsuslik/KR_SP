#pragma once

#include "common.h"
#include <string>

struct AsyncResult {
    AsyncStatus status;
    std::string data;
    StatusCode  db_code;
    std::string completion_time;
};

class AsyncManager {
    
public:
    AsyncManager();
    ~AsyncManager();

    std::string register_task();
    void update_task(const std::string& guid, AsyncStatus status, StatusCode code, const std::string& result_json);
    AsyncResult fetch_result(const std::string& guid);

    void set_session_db(int fd, const std::string& db_name);
    std::string get_session_db(int fd);
    void close_session(int fd);

    int find_idle_node() const;

    SharedMemoryLayout* getLayout() const;

private:
    SharedMemoryLayout* layout_ = nullptr;
    SharedTaskSlot* shared_array = nullptr;
    size_t total_size = 0;

    static std::string generate_guid_v4();
    SharedTaskSlot* find_free_slot();
};
