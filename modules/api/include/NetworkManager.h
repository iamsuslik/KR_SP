#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <string>
#include <vector>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>

class NetworkManager {
public:
    static constexpr int DEFAULT_PORT = 5432;
    static constexpr size_t MAX_MESSAGE_SIZE = 10 * 1024 * 1024; // 10MB Limit
    static constexpr int IO_TIMEOUT_MS = 5000;

    static bool sendExact(int sock, const void* data, size_t len) {
        const char* ptr = static_cast<const char*>(data);
        while (len > 0) {
            ssize_t sent = send(sock, ptr, len, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!waitSocket(sock, POLLOUT)) return false;
                    continue;
                }
                return false;
            }
            if (sent == 0) return false;
            ptr += sent; len -= sent;
        }
        return true;
    }

    static bool recvExact(int sock, void* data, size_t len) {
        char* ptr = static_cast<char*>(data);
        while (len > 0) {
            ssize_t rec = recv(sock, ptr, len, MSG_DONTWAIT);
            if (rec < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!waitSocket(sock, POLLIN)) return false;
                    continue;
                }
                return false;
            }
            if (rec == 0) return false;
            ptr += rec; len -= rec;
        }
        return true;
    }

    static void sendString(int socket, const std::string& msg) {
        uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
        if (sendExact(socket, &len, sizeof(len)) && !msg.empty()) {
            sendExact(socket, msg.c_str(), msg.size());
        }
    }

    static std::string receiveString(int socket) {
        uint32_t net_len = 0;
        if (!recvExact(socket, &net_len, sizeof(net_len))) return "";
        uint32_t len = ntohl(net_len);
        if (len == 0 || len > MAX_MESSAGE_SIZE) return "";
        std::vector<char> buf(len);
        return recvExact(socket, buf.data(), len) ? std::string(buf.begin(), buf.end()) : "";
    }

private:
    static bool waitSocket(int fd, short events) {
        pollfd pfd = {fd, events, 0};
        return poll(&pfd, 1, IO_TIMEOUT_MS) > 0 && (pfd.revents & events);
    }
};
#endif