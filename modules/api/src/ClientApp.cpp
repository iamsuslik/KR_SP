#include <errno.h>
#include <signal.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "NetworkManager.h"

using json = nlohmann::json;

#define CLR_RST "\033[0m"
#define CLR_ERR "\033[31m"
#define CLR_INF "\033[34m"
#define CLR_SQL "\033[32m"
#define CLR_WRN "\033[33m"

namespace Config {
const char* SERVER_IP = "127.0.0.1";
constexpr int POLL_INTERVAL_MS = 500;
constexpr int CLEANUP_WIDTH = 60;
}  // namespace Config

namespace Protocol {
const std::string ASYNC_PREFIX = "ASYNC_ID: ";
const std::string STATUS_PENDING = "Processing...";
const std::string CMD_CHECK = "CHECK ";
const std::string MARKER_EOF = "EOF_MARKER";
}  // namespace Protocol

volatile sig_atomic_t shutdown_flag = 0;
void handle_sigint(int) { shutdown_flag = 1; }

std::string g_session_token = "";

int connectToServer() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in serv_addr {};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NetworkManager::DEFAULT_PORT);
    inet_pton(AF_INET, Config::SERVER_IP, &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

bool isQueryComplete(const std::string& buffer) {
    bool in_quotes = false;
    for (char c : buffer) {
        if (c == '"') in_quotes = !in_quotes;
        if (c == ';' && !in_quotes) return true;
    }
    return false;
}

void pollForResult(const std::string& guid) {
    std::cout << CLR_INF << "[Async] Task submitted. Waiting for result... "
              << CLR_RST << std::flush;

    const char spinner[] = {'|', '/', '-', '\\'};
    int s_idx = 0;

    while (!shutdown_flag) {
        int poll_sock = connectToServer();
        if (poll_sock < 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(Config::POLL_INTERVAL_MS));
            continue;
        }

        NetworkManager::sendString(poll_sock, Protocol::CMD_CHECK + guid);
        std::string response = NetworkManager::receiveString(poll_sock);
        close(poll_sock);

        if (response != Protocol::STATUS_PENDING) {
            try {
                auto j = json::parse(response);
                if (j.contains("token")) {
                    g_session_token = j["token"].get<std::string>();
                }
            } catch (...) {
            }

            std::cout << "\r" << std::string(Config::CLEANUP_WIDTH, ' ')
                      << "\r";
            std::cout << CLR_INF << "[Result from " << guid << "]:" << CLR_RST
                      << std::endl;
            std::cout << response << std::endl;
            return;
        }

        std::cout << "\r" << CLR_INF << "[Async] Processing "
                  << spinner[s_idx++ % 4] << CLR_RST << std::flush;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(Config::POLL_INTERVAL_MS));
    }
}

void processExchange(const std::string& query) {
    if (query.empty()) return;

    int sock = connectToServer();
    if (sock < 0) {
        std::cerr << CLR_ERR << "[Error] Server is offline." << CLR_RST
                  << std::endl;
        return;
    }

    std::string payload = g_session_token + "@@@" + query;
    NetworkManager::sendString(sock, payload);
    std::string first_resp = NetworkManager::receiveString(sock);
    close(sock);

    if (first_resp.compare(0, Protocol::ASYNC_PREFIX.length(),
                           Protocol::ASYNC_PREFIX) == 0) {
        std::string guid = first_resp.substr(Protocol::ASYNC_PREFIX.length());
        pollForResult(guid);
    } else {
        try {
            auto j = json::parse(first_resp);
            if (j.contains("token")) {
                g_session_token = j["token"].get<std::string>();
            }
        } catch (...) {
        }
        std::cout << first_resp << std::endl;
    }
}

void runInteractive() {
    std::cout << CLR_INF << "DBMS Shell Connected (Async Mode). End with ';'."
              << CLR_RST << std::endl;

    std::string query_buffer;
    while (!shutdown_flag) {
        if (query_buffer.empty())
            std::cout << CLR_SQL << "sql> " << CLR_RST << std::flush;
        else
            std::cout << CLR_INF << "  -> " << CLR_RST << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line == "exit" || line == "quit") break;

        query_buffer += line + " ";

        if (isQueryComplete(query_buffer)) {
            processExchange(query_buffer);
            query_buffer.clear();
        }
    }
}

void runBatch(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << CLR_ERR << "[IO Error] Cannot open: " << path << CLR_RST
                  << std::endl;
        return;
    }

    std::string line, buffer;
    while (std::getline(file, line)) {
        if (line.empty() || line.compare(0, 2, "--") == 0) continue;
        buffer += line + " ";
        if (isQueryComplete(buffer)) {
            processExchange(buffer);
            buffer.clear();
        }
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_sigint);

    int test_sock = connectToServer();
    if (test_sock < 0) {
        std::cerr << CLR_ERR << "Fatal: Cannot connect to DBMS Server at "
                  << Config::SERVER_IP << CLR_RST << std::endl;
        return EXIT_FAILURE;
    }
    close(test_sock);

    if (argc > 1)
        runBatch(argv[1]);
    else
        runInteractive();

    return EXIT_SUCCESS;
}