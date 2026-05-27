#include "NetworkManager.h"
#include "SQLParser.h"
#include "HierarchyManager.h"
#include "TelemetryManager.h"
#include "AsyncManager.h"
#include "Logger.h"
#include "TableLockManager.h"
#include "common.h"

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <atomic>
#include <sys/wait.h>
#include <cstring>

static constexpr int POLL_TIMEOUT_MS = 1000;
static constexpr const char* CMD_CHECK = "CHECK ";
static constexpr size_t CMD_CHECK_LEN   = 6;


std::atomic<bool> g_keep_running{true};

static void handle_shutdown(int) { g_keep_running = false; }

static bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl != -1 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

int main() {
    signal(SIGINT, handle_shutdown);
    signal(SIGTERM, handle_shutdown);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    AsyncManager    async;
    TelemetryManager telemetry;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[CRITICAL] socket"); return EXIT_FAILURE; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(server_fd);

    sockaddr_in addr{};
    addr.sin_family= AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port  = htons(NetworkManager::DEFAULT_PORT);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[CRITICAL] bind"); return EXIT_FAILURE;
    }
    listen(server_fd, SOMAXCONN);

    std::vector<pollfd> fds;
    fds.push_back({server_fd, POLLIN, 0});

    std::cout << ">>> DBMS MULTI-PROCESS SERVER (mmap Shared Memory) ONLINE <<<" << std::endl;

    while (g_keep_running) {
        int pr = poll(fds.data(), static_cast<nfds_t>(fds.size()), POLL_TIMEOUT_MS);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        for (size_t i = 0; i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;

            if (fds[i].fd == server_fd) {
                sockaddr_in ca; socklen_t cl = sizeof(ca);
                int csock = accept(server_fd,
                                   reinterpret_cast<sockaddr*>(&ca), &cl);
                if (csock >= 0) {
                    set_nonblocking(csock);
                    fds.push_back({csock, POLLIN, 0});
                }
                continue;
            }

            int  cfd   = fds[i].fd;
            auto query = NetworkManager::receiveString(cfd);

            if (query.empty()) {
                async.close_session(cfd);
                close(cfd);
                fds.erase(fds.begin() + static_cast<long>(i));
                --i;
                continue;
            }

            if (query.compare(0, CMD_CHECK_LEN, CMD_CHECK) == 0) {
                std::string guid = query.substr(CMD_CHECK_LEN);
                if (!guid.empty() && guid.back() == ';') guid.pop_back();

                AsyncResult res = async.fetch_result(guid);
                std::string resp = (res.status == AsyncStatus::PENDING ||
                                    res.status == AsyncStatus::PROCESSING)
                                   ? "Processing..."
                                   : res.data;
                NetworkManager::sendString(cfd, resp);
                close(cfd);
                fds.erase(fds.begin() + static_cast<long>(i));
                --i;
                continue;
            }

            std::string guid       = async.register_task();
            std::string current_db = async.get_session_db(cfd);

            int pipefd[2];
            if (pipe(pipefd) == -1) {
                perror("[Error] pipe");
                NetworkManager::sendString(cfd, "ASYNC_ID: " + guid);
                close(cfd);
                fds.erase(fds.begin() + static_cast<long>(i));
                --i;
                continue;
            }

            NetworkManager::sendString(cfd, "ASYNC_ID: " + guid);

            pid_t pid = fork();
            if (pid < 0) {
                perror("[Error] fork");
                close(pipefd[0]); close(pipefd[1]);
            } else if (pid == 0) {
                close(pipefd[1]);

                WorkerContext ctx{};
                ssize_t r = ::read(pipefd[0], &ctx, sizeof(ctx));
                close(pipefd[0]);
                if (r <= 0) ::exit(EXIT_FAILURE);

                for (auto& pfd : fds) close(pfd.fd);

                HierarchyManager hm;
                if (ctx.current_db[0] != '\0')
                    hm.useDatabase(std::string(ctx.current_db));

                SQLParser parser;
                std::string result_buf;
                auto output_cb = [&](const std::string& s) { result_buf += s; };

                auto t0 = std::chrono::high_resolution_clock::now();
                Result res = parser.process(std::string(ctx.query), hm, ctx.client_fd, output_cb);
                auto t1 = std::chrono::high_resolution_clock::now();
                long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

                std::string new_db = hm.getCurrentDB();
                if (!new_db.empty() && new_db != std::string(ctx.current_db)) {
                    async.set_session_db(ctx.client_fd, new_db);
                }

                async.update_task(std::string(ctx.guid),
                                  AsyncStatus::COMPLETED,
                                  res.code,
                                  result_buf);

                Logger::log(std::string(ctx.query),
                            res.isOk() ? "SUCCESS" : "ERROR", ms);

                ::exit(EXIT_SUCCESS);
            } else {
                close(pipefd[0]);

                WorkerContext ctx{};
                std::strncpy(ctx.guid, guid.c_str(), 36);
                std::strncpy(ctx.query, query.c_str(), sizeof(ctx.query) - 1);
                std::strncpy(ctx.current_db, current_db.c_str(), MAX_NAME_LEN - 1);
                ctx.client_fd = cfd;
                ::write(pipefd[1], &ctx, sizeof(ctx));
                close(pipefd[1]);

                telemetry.recordQuery(0, false);
            }

            close(cfd);
            fds.erase(fds.begin() + static_cast<long>(i));
            --i;
        }
    }

    for (auto& pfd : fds) close(pfd.fd);
    std::cout << "[Server] Shutdown complete.\n";
    return EXIT_SUCCESS;
}