#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "AsyncManager.h"
#include "AuthManager.h"
#include "HierarchyManager.h"
#include "Logger.h"
#include "NetworkManager.h"
#include "SQLParser.h"
#include "TableLockManager.h"
#include "TelemetryManager.h"
#include "common.h"

static constexpr int POLL_TIMEOUT_MS = 1000;
static constexpr int HEARTBEAT_SEC = 10;
static constexpr const char* CMD_CHECK = "CHECK ";
static constexpr size_t CMD_CHECK_LEN = 6;

static std::atomic<bool> g_keep_running{true};
static void handle_shutdown(int) { g_keep_running = false; }

static bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl != -1 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

static int64_t now_sec() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

static int shard_node(const std::string& table_name) {
    if (table_name.empty()) return 0;
    return static_cast<int>(std::hash<std::string>{}(table_name) % N_NODES);
}

static std::string extract_table_name(const std::string& query) {
    static const std::vector<std::string> kws = {"FROM", "INTO", "UPDATE",
                                                 "TABLE"};

    std::string upper(query.size(), '\0');
    for (size_t i = 0; i < query.size(); ++i)
        upper[i] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(query[i])));

    for (const auto& kw : kws) {
        size_t pos = upper.find(kw);
        if (pos == std::string::npos) continue;
        pos += kw.size();

        while (pos < query.size() && query[pos] == ' ') ++pos;

        size_t start = pos;
        while (pos < query.size() &&
               (std::isalnum(static_cast<unsigned char>(query[pos])) ||
                query[pos] == '_' || query[pos] == '.')) {
            ++pos;
        }
        if (pos > start) return query.substr(start, pos - start);
    }
    return "";
}

[[noreturn]] static void worker_node_main(int node_id,
                                          SharedMemoryLayout* layout,
                                          AsyncManager& async) {
    g_current_node_id = node_id;
    NodeControl& ctrl = layout->nodes[node_id];

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_DFL);

    HierarchyManager hm;

    while (true) {
        while (sem_wait(&ctrl.sem_task) != 0) {
            if (errno == EINTR) continue;
            perror("[Worker] sem_wait failed");
            ::exit(EXIT_FAILURE);
        }

        std::string raw_msg(ctrl.query_buffer);
        std::string guid(ctrl.task_guid);

        size_t delim = raw_msg.find("@@@");
        std::string token = "";
        std::string query = raw_msg;
        if (delim != std::string::npos) {
            token = raw_msg.substr(0, delim);
            query = raw_msg.substr(delim + 3);
        }

        std::string db = async.get_session_db(token);
        if (!db.empty()) hm.useDatabase(db);

        SQLParser parser;
        std::string result_buf;
        auto output_cb = [&](const std::string& s) { result_buf += s; };

        auto t0 = std::chrono::high_resolution_clock::now();
        Result res = parser.process(query, hm, token, output_cb);
        auto t1 = std::chrono::high_resolution_clock::now();
        long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count();

        async.update_task(guid, AsyncStatus::COMPLETED, res.code, result_buf);

        std::string new_db = hm.getCurrentDB();
        if (!new_db.empty() && !token.empty()) {
            async.set_session_db(token, new_db);
        }

        Logger::log(query, res.isOk() ? "SUCCESS" : "ERROR", ms);

        ctrl.last_seen = now_sec();
        ctrl.is_busy = false;

        sem_post(&ctrl.sem_ready);
    }
}

static void spawn_worker(int node_id, SharedMemoryLayout* layout,
                         AsyncManager& async) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[Master] fork failed");
        return;
    }
    if (pid == 0) {
        worker_node_main(node_id, layout, async);
    }
    layout->nodes[node_id].worker_pid = pid;
    layout->nodes[node_id].last_seen = now_sec();
    layout->nodes[node_id].is_busy = false;
    std::cout << "[Master] Worker " << node_id << " spawned, PID=" << pid
              << "\n";
}

int main() {
    signal(SIGINT, handle_shutdown);
    signal(SIGTERM, handle_shutdown);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    AsyncManager async;
    g_async_manager = &async;
    TelemetryManager telemetry;
    SharedMemoryLayout* layout = async.getLayout();

    HierarchyManager hm_init;
    AuthManager::initSystem(hm_init);

    for (int i = 0; i < N_NODES; ++i) {
        spawn_worker(i, layout, async);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[CRITICAL] socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(server_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(NetworkManager::DEFAULT_PORT);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[CRITICAL] bind");
        return EXIT_FAILURE;
    }
    listen(server_fd, SOMAXCONN);

    std::vector<pollfd> fds;
    fds.push_back({server_fd, POLLIN, 0});

    std::cout
        << ">>> DBMS CLUSTER SERVER (Shared Memory + Semaphores) ONLINE <<<\n";
    std::cout << "    Nodes: " << N_NODES
              << ", Heartbeat timeout: " << HEARTBEAT_SEC << "s\n";

    while (g_keep_running) {
        int pr =
            poll(fds.data(), static_cast<nfds_t>(fds.size()), POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        {
            int64_t cur = now_sec();
            for (int i = 0; i < N_NODES; ++i) {
                NodeControl& ctrl = layout->nodes[i];

                bool alive =
                    (ctrl.worker_pid > 0) && (kill(ctrl.worker_pid, 0) == 0);

                if (!alive ||
                    (ctrl.is_busy && (cur - ctrl.last_seen) > HEARTBEAT_SEC)) {
                    if (ctrl.worker_pid > 0) {
                        std::cerr << "[Heartbeat] Worker " << i
                                  << " (PID=" << ctrl.worker_pid
                                  << ") unresponsive. Restarting...\n";
                        kill(ctrl.worker_pid, SIGKILL);
                        waitpid(ctrl.worker_pid, nullptr, WNOHANG);
                    }
                    sem_destroy(&ctrl.sem_task);
                    sem_destroy(&ctrl.sem_ready);
                    sem_init(&ctrl.sem_task, 1, 0);
                    sem_init(&ctrl.sem_ready, 1, 1);

                    spawn_worker(i, layout, async);
                }
            }
        }

        for (size_t i = 0; i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;

            if (fds[i].fd == server_fd) {
                sockaddr_in ca;
                socklen_t cl = sizeof(ca);
                int csock =
                    accept(server_fd, reinterpret_cast<sockaddr*>(&ca), &cl);
                if (csock >= 0) {
                    set_nonblocking(csock);
                    fds.push_back({csock, POLLIN, 0});
                }
                continue;
            }

            int cfd = fds[i].fd;
            auto raw_message = NetworkManager::receiveString(cfd);

            if (raw_message.empty()) {
                close(cfd);
                fds.erase(fds.begin() + static_cast<long>(i));
                --i;
                continue;
            }

            if (raw_message.compare(0, CMD_CHECK_LEN, CMD_CHECK) == 0) {
                std::string guid = raw_message.substr(CMD_CHECK_LEN);
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

            std::string guid = async.register_task();

            size_t delim = raw_message.find("@@@");
            std::string query = raw_message;
            if (delim != std::string::npos) {
                query = raw_message.substr(delim + 3);
            }

            std::string table_name = extract_table_name(query);
            int target_node = shard_node(table_name);

            NodeControl& ctrl = layout->nodes[target_node];

            while (sem_wait(&ctrl.sem_ready) != 0) {
                if (errno == EINTR) continue;
                break;
            }

            std::strncpy(ctrl.query_buffer, raw_message.c_str(),
                         NODE_QUERY_BUFFER - 1);
            ctrl.query_buffer[NODE_QUERY_BUFFER - 1] = '\0';

            std::strncpy(ctrl.task_guid, guid.c_str(), 36);
            ctrl.task_guid[36] = '\0';

            ctrl.is_busy = true;
            ctrl.last_seen = now_sec();

            sem_post(&ctrl.sem_task);

            NetworkManager::sendString(cfd, "ASYNC_ID: " + guid);
            telemetry.recordQuery(0, false);

            close(cfd);
            fds.erase(fds.begin() + static_cast<long>(i));
            --i;
        }
    }

    for (int i = 0; i < N_NODES; ++i) {
        if (layout->nodes[i].worker_pid > 0) {
            kill(layout->nodes[i].worker_pid, SIGTERM);
        }
    }

    for (auto& pfd : fds) close(pfd.fd);
    std::cout << "[Master] Shutdown complete.\n";
    return EXIT_SUCCESS;
}
