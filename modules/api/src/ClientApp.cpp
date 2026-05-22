#include "NetworkManager.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>

// UI Colors
#define CLR_RST  "\033[0m"
#define CLR_ERR  "\033[31m"
#define CLR_INF  "\033[34m"
#define CLR_SQL  "\033[32m"
#define CLR_WRN  "\033[33m"

volatile sig_atomic_t shutdown_flag = 0;
void handle_sigint(int) { shutdown_flag = 1; }

/**
 * Проверка завершенности SQL запроса.
 * Игнорирует точки с запятой внутри кавычек.
 */
bool isQueryComplete(const std::string& buffer) {
    bool in_quotes = false;
    for (char c : buffer) {
        if (c == '"') in_quotes = !in_quotes;
        if (c == ';' && !in_quotes) return true;
    }
    return false;
}

/**
 * Выполнение запроса и обработка потокового ответа
 */
void processExchange(int sock, const std::string& query) {
    if (query.empty()) return;

    NetworkManager::sendString(sock, query);

    while (true) {
        std::string part = NetworkManager::receiveString(sock);
        
        if (part == "EOF_MARKER") break;
        
        if (part.empty()) {
            std::cerr << CLR_ERR << "\n[Error] Connection closed by remote host." << CLR_RST << std::endl;
            exit(EXIT_FAILURE);
        }

        // Вывод ошибок сервера в STDERR красным, данных - в STDOUT как есть
        if (part.find("[Error]") == 0) {
            std::cerr << CLR_ERR << part << CLR_RST << std::endl;
        } else {
            std::cout << part << std::flush;
        }
    }
    std::cout << std::endl;
}

/**
 * Профессиональный интерактивный режим с использованием select()
 */
void runInteractive(int sock) {
    std::cout << CLR_INF << "DBMS Shell Connected. Use ';' to execute queries." << CLR_RST << std::endl;

    std::string query_buffer;
    char input_buf[1024];

    while (!shutdown_flag) {
        if (query_buffer.empty()) std::cout << CLR_SQL << "sql> " << CLR_RST << std::flush;
        else std::cout << CLR_INF << "  -> " << CLR_RST << std::flush;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sock, &read_fds);

        // Ждем либо ввода юзера, либо данных/обрыва от сервера
        int s = select(sock + 1, &read_fds, nullptr, nullptr, nullptr);

        if (s < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 1. Проверка состояния сервера (не упал ли?)
        if (FD_ISSET(sock, &read_fds)) {
            char peek_buf[1];
            if (recv(sock, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                std::cerr << CLR_ERR << "\n[Fatal] Server disconnected." << CLR_RST << std::endl;
                return;
            }
        }

        // 2. Обработка ввода пользователя
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            std::string line;
            if (!std::getline(std::cin, line)) break;
            if (line == "exit" || line == "quit") break;

            query_buffer += line + " ";

            if (isQueryComplete(query_buffer)) {
                processExchange(sock, query_buffer);
                query_buffer.clear();
            }
        }
    }
}

/**
 * Пакетный режим (обработка скриптов)
 */
void runBatch(int sock, const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << CLR_ERR << "[IO Error] Cannot open: " << path << CLR_RST << std::endl;
        return;
    }

    std::string line, buffer;
    while (std::getline(file, line)) {
        if (line.empty() || line.compare(0, 2, "--") == 0) continue;
        buffer += line + " ";
        if (isQueryComplete(buffer)) {
            processExchange(sock, buffer);
            buffer.clear();
        }
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_sigint);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket fail");
        return EXIT_FAILURE;
    }

    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NetworkManager::DEFAULT_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << CLR_ERR << "Connection refused. Ensure Server is running." << CLR_RST << std::endl;
        close(sock);
        return EXIT_FAILURE;
    }

    if (argc > 1) runBatch(sock, argv[1]);
    else runInteractive(sock);

    close(sock);
    return EXIT_SUCCESS;
}