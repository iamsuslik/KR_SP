#include "NetworkManager.h"
#include "SQLParser.h"
#include "HierarchyManager.h"
#include "TelemetryManager.h"
#include "AsyncManager.h"
#include "Logger.h"

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

// Глобальный флаг состояния для безопасной остановки сервера (Graceful Shutdown)
std::atomic<bool> g_keep_running{true};

/**
 * Обработчик системных сигналов завершения.
 */
void handle_shutdown_signal(int sig_num) {
    g_keep_running = false;
}

/**
 * Переводит файловый дескриптор в неблокирующий режим работы.
 */
bool set_fd_nonblocking(int fd) {
    int current_flags = fcntl(fd, F_GETFL, 0);
    if (current_flags == -1) return false;
    return fcntl(fd, F_SETFL, current_flags | O_NONBLOCK) == 0;
}

int main() {
    // 1. НАСТРОЙКА ОКРУЖЕНИЯ
    signal(SIGINT,  handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGPIPE, SIG_IGN); // Игнорируем обрывы сокетов, чтобы сервер не падал

    // 2. ИНИЦИАЛИЗАЦИЯ КОМПОНЕНТОВ
    HierarchyManager hm;
    SQLParser parser;
    TelemetryManager telemetry;
    AsyncManager async_pool(4); // Пул воркеров

    // Интеграция движка
    async_pool.db_engine = [&](const std::string& query, std::function<void(const std::string&)> output_cb) {
        // Здесь мы передаем телеметрии данные об успехе/ошибке
        auto start = std::chrono::high_resolution_clock::now();
        
        // Вызов парсера (Даша должна добавить поддержку output_cb)
        Result res = parser.process(query, hm, output_cb); 
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        telemetry.recordQuery(duration, !res.isOk());
        Logger::log(query, res.isOk() ? "SUCCESS" : "ERROR", duration);
        
        return res;
    };

    // 3. ПОДГОТОВКА СЕТИ
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[CRITICAL] Socket fail");
        return EXIT_FAILURE;
    }

    int socket_opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_opt, sizeof(socket_opt));

    if (!set_fd_nonblocking(server_fd)) {
        perror("[CRITICAL] Non-blocking fail");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NetworkManager::DEFAULT_PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[CRITICAL] Bind fail");
        return EXIT_FAILURE;
    }

    listen(server_fd, SOMAXCONN);

    // 4. ИНИЦИАЛИЗАЦИЯ ТАБЛИЦЫ POLL
    std::vector<pollfd> poll_fds;
    poll_fds.push_back({server_fd, POLLIN, 0});

    std::cout << ">>> DBMS SERVER ONLINE (Non-blocking Mode) <<<" << std::endl;

    // 5. EVENT LOOP (Главный поток)
    while (g_keep_running) {
        // Таймаут 1000мс позволяет циклу проверять g_keep_running раз в секунду
        int poll_result = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), 1000);
        
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (poll_result == 0) continue; 

        for (size_t i = 0; i < poll_fds.size(); ++i) {
            if (!(poll_fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;

            // НОВОЕ СОЕДИНЕНИЕ
            if (poll_fds[i].fd == server_fd) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                    
                    if (client_sock < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }

                    if (set_fd_nonblocking(client_sock)) {
                        poll_fds.push_back({client_sock, POLLIN, 0});
                        std::clog << "[Network] Accepted FD: " << client_sock << std::endl;
                    } else {
                        close(client_sock);
                    }
                }
            } 
            // ДАННЫЕ ОТ КЛИЕНТА
            else {
                int client_fd = poll_fds[i].fd;
                
                // Проверка на обрыв
                if (poll_fds[i].revents & (POLLERR | POLLHUP)) {
                    std::clog << "[Network] Disconnected FD: " << client_fd << std::endl;
                    close(client_fd);
                    poll_fds.erase(poll_fds.begin() + i);
                    i--;
                    continue;
                }

                // --- ИСПРАВЛЕНИЕ БЛОКИРОВКИ ---
                // Вместо вызова NetworkManager::receiveString(client_fd), который заставит
                // сервер ждать байты, мы просто "выбрасываем" сокет в пул воркеров.
                // Воркер сам дождется данных, сам обработает и сам закроет сокет.
                
                std::clog << "[Network] Task delegated to Worker Pool. FD: " << client_fd << std::endl;
                
                // Передаем сокет в асинхронную очередь
                async_pool.enqueue_socket(client_fd);

                // ВАЖНО: Удаляем сокет из мониторинга poll в главном потоке, 
                // так как теперь им владеет поток воркера.
                poll_fds.erase(poll_fds.begin() + i);
                i--;
            }
        }

        // Периодический вывод телеметрии в лог сервера (раз в 30 секунд или по событию)
        // telemetry.printMetrics(); 
    }

    // 6. ЗАВЕРШЕНИЕ
    std::cout << "[System] Closing server..." << std::endl;
    for (const auto& entry : poll_fds) close(entry.fd);
    
    return EXIT_SUCCESS;
}