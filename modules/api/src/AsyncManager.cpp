#include "AsyncManager.h"
#include <random>
#include <cstring>
#include <chrono>
#include <ctime>
#include <iostream>
#include <sys/mman.h>

AsyncManager::AsyncManager() {

    total_size = sizeof(SharedTaskSlot) * MAX_SHARED_TASKS;

    shared_array = (SharedTaskSlot*)mmap(NULL, total_size, 
                                        PROT_READ | PROT_WRITE, 
                                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (shared_array == MAP_FAILED) {
        perror("[Critical] mmap failed");
        exit(EXIT_FAILURE);
    }

    std::memset(shared_array, 0, total_size);
}

AsyncManager::~AsyncManager() {
    if (shared_array && shared_array != MAP_FAILED) {
        munmap(shared_array, total_size);
    }
}

std::string AsyncManager::generate_guid_v4() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    
    uint32_t d[4] = {dis(gen), (dis(gen) & 0xFFFF0FFF) | 0x4000, 
                     (dis(gen) & 0x3FFFFFFF) | 0x80000000, dis(gen)};
    
    char b[37];
    std::snprintf(b, sizeof(b), "%08x-%04x-%04x-%04x-%08x%04x", 
                 d[0], d[1] >> 16, d[1] & 0xFFFF, d[2] >> 16, d[2] & 0xFFFF, d[3]);
    return std::string(b);
}

SharedTaskSlot* AsyncManager::find_free_slot() {
    for (int i = 0; i < MAX_SHARED_TASKS; ++i) {
        if (!shared_array[i].is_used) return &shared_array[i];
    }
    static int last_overwritten = 0;
    last_overwritten = (last_overwritten + 1) % MAX_SHARED_TASKS;
    return &shared_array[last_overwritten];
}

std::string AsyncManager::register_task() {
    SharedTaskSlot* slot = find_free_slot();
    std::string guid = generate_guid_v4();

    std::memset(slot, 0, sizeof(SharedTaskSlot));
    
    std::strncpy(slot->guid, guid.c_str(), 36);
    slot->status = AsyncStatus::PENDING;
    slot->is_used = true;

    return guid;
}

void AsyncManager::update_task(const std::string& guid, AsyncStatus status, StatusCode code, const std::string& result_json) {
    for (int i = 0; i < MAX_SHARED_TASKS; ++i) {
        if (shared_array[i].is_used && std::strncmp(shared_array[i].guid, guid.c_str(), 36) == 0) {
            
            shared_array[i].status = status;
            shared_array[i].db_code = code;

            std::strncpy(shared_array[i].result_data, result_json.c_str(), SHARED_RESULT_SIZE - 1);
            shared_array[i].result_data[SHARED_RESULT_SIZE - 1] = '\0';

            auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            char t_buf[26];
            #ifdef _WIN32
                ctime_s(t_buf, sizeof(t_buf), &now);
            #else
                ctime_r(&now, t_buf);
            #endif
            std::strncpy(shared_array[i].completion_time, t_buf, 25);
            
            return;
        }
    }
}

AsyncResult AsyncManager::fetch_result(const std::string& guid) {
    for (int i = 0; i < MAX_SHARED_TASKS; ++i) {
        if (shared_array[i].is_used && std::strncmp(shared_array[i].guid, guid.c_str(), 36) == 0) {
            return {
                shared_array[i].status,
                std::string(shared_array[i].result_data),
                shared_array[i].db_code,
                std::string(shared_array[i].completion_time)
            };
        }
    }
    return {AsyncStatus::FAILED, "GUID_NOT_FOUND", StatusCode::NOT_FOUND, ""};
}

void AsyncManager::set_session_db(int fd, const std::string& db_name) {
    SharedMemoryLayout* layout = (SharedMemoryLayout*)shared_array;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (layout->sessions[i].is_active && layout->sessions[i].client_fd == fd) {
            std::strncpy(layout->sessions[i].current_db, db_name.c_str(), MAX_NAME_LEN - 1);
            return;
        }
    }
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!layout->sessions[i].is_active) {
            layout->sessions[i].client_fd = fd;
            layout->sessions[i].is_active = true;
            std::strncpy(layout->sessions[i].current_db, db_name.c_str(), MAX_NAME_LEN - 1);
            return;
        }
    }
}

std::string AsyncManager::get_session_db(int fd) {
    SharedMemoryLayout* layout = (SharedMemoryLayout*)shared_array;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (layout->sessions[i].is_active && layout->sessions[i].client_fd == fd) {
            return std::string(layout->sessions[i].current_db);
        }
    }
    return "";
}

void AsyncManager::close_session(int fd) {
    SharedMemoryLayout* layout = (SharedMemoryLayout*)shared_array;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (layout->sessions[i].is_active && layout->sessions[i].client_fd == fd) {
            layout->sessions[i].is_active = false;
            return;
        }
    }
}