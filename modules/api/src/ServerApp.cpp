#include "NetworkManager.h"
#include "SQLParser.h"
#include "HierarchyManager.h"
#include "TelemetryManager.h"
#include "AsyncManager.h"
#include "Logger.h"
#include "TableLockManager.h"

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <atomic>
#include <thread>
TableLockManager g_lock_manager;
// Константы конфигурации
static constexpr int POLL_TIMEOUT_MS = 1000;
static constexpr const char* CMD_CHECK = "CHECK ";

// Глобальный флаг состояния для безопасной остановки сервера
std::atomic<bool> g_keep_running{true};

void handle_shutdown_signal(int sig_num) {
    g_keep_running = false;
}

bool set_fd_nonblocking(int fd) {
    int current_flags = fcntl(fd, F_GETFL, 0);
    if (current_flags == -1) return false;
    return fcntl(fd, F_SETFL, current_flags | O_NONBLOCK) == 0;
}

int main() {
    // 1. НАСТРОЙКА ОКРУЖЕНИЯ
    signal(SIGINT,  handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);

    // 2. ИНИЦИАЛИЗАЦИЯ КОМПОНЕНТОВ
    HierarchyManager hm;
    SQLParser parser;
    TelemetryManager telemetry;
    
    // Динамический пул воркеров
    size_t pool_size = std::thread::hardware_concurrency();
    AsyncManager async_pool(pool_size > 0 ? pool_size : 4); 

    async_pool.db_engine = [&](const std::string& query, std::function<void(const std::string&)> output_cb) {
        auto start = std::chrono::high_resolution_clock::now();
        Result res = parser.process(query, hm, output_cb); 
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        telemetry.recordQuery(duration, !res.isOk());
        Logger::log(query, res.isOk() ? "SUCCESS" : "ERROR", duration);
        return res;
    };

    // 3. ПОДГОТОВКА СЕТИ
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[CRITICAL] Socket fail"); return EXIT_FAILURE; }

    int socket_opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_opt, sizeof(socket_opt));
    set_fd_nonblocking(server_fd);

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NetworkManager::DEFAULT_PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[CRITICAL] Bind fail");
        return EXIT_FAILURE;
    }
    listen(server_fd, SOMAXCONN);

    std::vector<pollfd> poll_fds;
    poll_fds.push_back({server_fd, POLLIN, 0});

    std::cout << ">>> DBMS SERVER ONLINE (Pool: " << pool_size << " threads) <<<" << std::endl;

    // 4. EVENT LOOP
    while (g_keep_running) {
        int poll_result = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), POLL_TIMEOUT_MS);
        
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (size_t i = 0; i < poll_fds.size(); ++i) {
            if (!(poll_fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;

            if (poll_fds[i].fd == server_fd) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_sock < 0) break;
                    
                    set_fd_nonblocking(client_sock);
                    poll_fds.push_back({client_sock, POLLIN, 0});
                }
            } else {
                int client_fd = poll_fds[i].fd;
                std::string query = NetworkManager::receiveString(client_fd);
                
                if (query.empty()) {
                    close(client_fd);
                    poll_fds.erase(poll_fds.begin() + i);
                    i--;
                    continue;
                }

                // Логика асинхронности через команду CHECK
                if (query.substr(0, std::string(CMD_CHECK).size()) == CMD_CHECK) {
                    std::string guid = query.substr(std::string(CMD_CHECK).size());
                    if (!guid.empty() && guid.back() == ';') guid.pop_back();
                    
                    auto res = async_pool.fetch_result(guid);
                    NetworkManager::sendString(client_fd, res.data.empty() ? "Processing..." : res.data);
                } else {
                    std::string guid = async_pool.enqueue(query);
                    NetworkManager::sendString(client_fd, "ASYNC_ID: " + guid);
                    NetworkManager::sendString(client_fd, "EOF_MARKER");
                }

                close(client_fd);
                poll_fds.erase(poll_fds.begin() + i);
                i--;
            }
        }
    }

    for (const auto& entry : poll_fds) close(entry.fd);
    return EXIT_SUCCESS;
}
