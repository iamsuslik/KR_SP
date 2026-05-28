#include "AsyncManager.h"

#include <sys/mman.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>

AsyncManager* g_async_manager = nullptr;

AsyncManager::AsyncManager() {
    total_size = sizeof(SharedMemoryLayout);

    layout_ = static_cast<SharedMemoryLayout*>(
        mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    if (layout_ == MAP_FAILED) {
        perror("[Critical] mmap failed");
        exit(EXIT_FAILURE);
    }

    std::memset(layout_, 0, total_size);

    for (int i = 0; i < N_NODES; ++i) {
        if (sem_init(&layout_->nodes[i].sem_task, /*pshared=*/1, 0) != 0) {
            perror("[Critical] sem_init(sem_task)");
            exit(EXIT_FAILURE);
        }
        if (sem_init(&layout_->nodes[i].sem_ready, /*pshared=*/1, 1) != 0) {
            perror("[Critical] sem_init(sem_ready)");
            exit(EXIT_FAILURE);
        }
        layout_->nodes[i].worker_pid = -1;
        layout_->nodes[i].is_busy = false;
        layout_->nodes[i].last_seen = 0;
    }

    shared_array = layout_->tasks;
}

AsyncManager::~AsyncManager() {
    if (layout_ && layout_ != MAP_FAILED) {
        for (int i = 0; i < N_NODES; ++i) {
            sem_destroy(&layout_->nodes[i].sem_task);
            sem_destroy(&layout_->nodes[i].sem_ready);
        }
        munmap(layout_, total_size);
    }
}

int AsyncManager::find_idle_node() const {
    for (int i = 0; i < N_NODES; ++i) {
        if (sem_trywait(&layout_->nodes[i].sem_ready) == 0) {
            return i;
        }
    }
    return -1;
}

SharedMemoryLayout* AsyncManager::getLayout() const { return layout_; }

std::string AsyncManager::generate_guid_v4() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);

    uint32_t d[4] = {dis(gen), (dis(gen) & 0xFFFF0FFF) | 0x4000,
                     (dis(gen) & 0x3FFFFFFF) | 0x80000000, dis(gen)};

    char b[37];
    std::snprintf(b, sizeof(b), "%08x-%04x-%04x-%04x-%08x%04x", d[0],
                  d[1] >> 16, d[1] & 0xFFFF, d[2] >> 16, d[2] & 0xFFFF, d[3]);
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

void AsyncManager::update_task(const std::string& guid, AsyncStatus status,
                               StatusCode code,
                               const std::string& result_json) {
    for (int i = 0; i < MAX_SHARED_TASKS; ++i) {
        if (shared_array[i].is_used &&
            std::strncmp(shared_array[i].guid, guid.c_str(), 36) == 0) {
            shared_array[i].status = status;
            shared_array[i].db_code = code;

            std::strncpy(shared_array[i].result_data, result_json.c_str(),
                         SHARED_RESULT_SIZE - 1);

            auto now_time = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            std::strncpy(shared_array[i].completion_time, std::ctime(&now_time),
                         25);

            return;
        }
    }
}

AsyncResult AsyncManager::fetch_result(const std::string& guid) {
    for (int i = 0; i < MAX_SHARED_TASKS; ++i) {
        if (shared_array[i].is_used &&
            std::strncmp(shared_array[i].guid, guid.c_str(), 36) == 0) {
            return {shared_array[i].status,
                    std::string(shared_array[i].result_data),
                    shared_array[i].db_code,
                    std::string(shared_array[i].completion_time)};
        }
    }
    return {AsyncStatus::FAILED, "GUID_NOT_FOUND", StatusCode::NOT_FOUND, ""};
}

void AsyncManager::set_session_db(const std::string& token,
                                  const std::string& db_name) {
    if (token.empty()) return;
    auto& sessions = layout_->sessions;

    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].is_active &&
            std::strncmp(sessions[i].session_token, token.c_str(), 255) == 0) {
            std::strncpy(sessions[i].current_db, db_name.c_str(),
                         MAX_NAME_LEN - 1);
            sessions[i].current_db[MAX_NAME_LEN - 1] = '\0';
            return;
        }
    }

    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!sessions[i].is_active) {
            sessions[i].is_active = true;
            std::strncpy(sessions[i].session_token, token.c_str(), 255);
            sessions[i].session_token[255] = '\0';
            std::strncpy(sessions[i].current_db, db_name.c_str(),
                         MAX_NAME_LEN - 1);
            sessions[i].current_db[MAX_NAME_LEN - 1] = '\0';
            return;
        }
    }
}

std::string AsyncManager::get_session_db(const std::string& token) {
    if (token.empty()) return "";
    auto& sessions = layout_->sessions;

    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].is_active &&
            std::strncmp(sessions[i].session_token, token.c_str(), 255) == 0) {
            return std::string(sessions[i].current_db);
        }
    }
    return "";
}

void AsyncManager::close_session(const std::string& token) {
    if (token.empty()) return;
    auto& sessions = layout_->sessions;

    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].is_active &&
            std::strncmp(sessions[i].session_token, token.c_str(), 255) == 0) {
            sessions[i].is_active = false;
            std::memset(sessions[i].session_token, 0, 256);
            std::memset(sessions[i].current_db, 0, MAX_NAME_LEN);
            return;
        }
    }
}

// void AsyncManager::set_session_user(int fd, const std::string& username) {
//     for (int i = 0; i < MAX_SESSIONS; ++i) {
//         layout_->sessions[i].is_active = false;
//     }
//     layout_->sessions[0].client_fd = fd;
//     layout_->sessions[0].is_active = true;
//     std::strncpy(layout_->sessions[0].current_user, username.c_str(),
//     MAX_NAME_LEN - 1);
// }

// std::string AsyncManager::get_session_user(int fd) {
//     for (int i = 0; i < MAX_SESSIONS; ++i) {
//         if (layout_->sessions[i].is_active) {
//             return std::string(layout_->sessions[i].current_user);
//         }
//     }
//     return "";
// }
