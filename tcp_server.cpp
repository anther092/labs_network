#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

#define MAX_PAYLOAD 1024

struct Message {
    uint32_t length;
    uint8_t  type;
    char     payload[MAX_PAYLOAD];
};

enum {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

static ssize_t recv_all(int fd, void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::recv(fd, static_cast<char*>(buf) + off, n - off, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(off);
}

static ssize_t send_all(int fd, const void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t s = ::send(fd, static_cast<const char*>(buf) + off, n - off, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += static_cast<size_t>(s);
    }
    return static_cast<ssize_t>(off);
}

static int send_message(int fd, uint8_t type, const void* payload, uint32_t payload_len) {
    if (payload_len > MAX_PAYLOAD) return -1;

    uint32_t length = 1u + payload_len;
    uint32_t net_len = htonl(length);

    if (send_all(fd, &net_len, sizeof(net_len)) != (ssize_t)sizeof(net_len)) return -1;
    if (send_all(fd, &type, sizeof(type)) != (ssize_t)sizeof(type)) return -1;
    if (payload_len > 0) {
        if (send_all(fd, payload, payload_len) != (ssize_t)payload_len) return -1;
    }
    return 0;
}

static int recv_message(int fd, Message& msg) {
    uint32_t net_len = 0;
    ssize_t r = recv_all(fd, &net_len, sizeof(net_len));
    if (r == 0) return 0;
    if (r < 0) return -1;

    uint32_t length = ntohl(net_len);
    if (length < 1u || length > 1u + MAX_PAYLOAD) return -2;

    uint8_t type = 0;
    r = recv_all(fd, &type, sizeof(type));
    if (r == 0) return 0;
    if (r < 0) return -1;

    uint32_t payload_len = length - 1u;
    if (payload_len > 0) {
        r = recv_all(fd, msg.payload, payload_len);
        if (r == 0) return 0;
        if (r < 0) return -1;
    }

    if (payload_len >= MAX_PAYLOAD) payload_len = MAX_PAYLOAD - 1;
    msg.payload[payload_len] = '\0';
    msg.length = length;
    msg.type = type;
    return 1;
}

static std::string addr_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    uint16_t port = ntohs(addr.sin_port);
    return std::string(ip) + ":" + std::to_string(port);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int port = std::stoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port.\n";
        return 1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(s, (sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind");
        close(s);
        return 1;
    }
    if (listen(s, 1) < 0) {
        perror("listen");
        close(s);
        return 1;
    }

    sockaddr_in cli{};
    socklen_t cli_len = sizeof(cli);
    int c = accept(s, (sockaddr*)&cli, &cli_len);
    if (c < 0) {
        perror("accept");
        ::close(s);
        return 1;
    }

    std::string cli_str = addr_to_string(cli);
    std::cout << "Client connected\n";

    Message msg{};
    int rc = recv_message(c, msg);
    if (rc <= 0) {
        std::cout << "Client disconnected\n";
        ::close(c);
        ::close(s);
        return 0;
    }
    if (msg.type != MSG_HELLO) {
        std::cerr << "Expected MSG_HELLO, got type=" << (unsigned)msg.type << "\n";
        ::close(c);
        ::close(s);
        return 1;
    }

    std::cout << "[" << cli_str << "]: " << msg.payload << "\n";

    std::string welcome = "Welcome " + cli_str;
    if (send_message(c, MSG_WELCOME, welcome.c_str(), (uint32_t)welcome.size()) != 0) {
        perror("send WELCOME");
        ::close(c);
        ::close(s);
        return 1;
    }

    while (true) {
        rc = recv_message(c, msg);
        if (rc <= 0) {
            std::cout << "Client disconnected\n";
            break;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << "[" << cli_str << "]: " << msg.payload << "\n";
        } else if (msg.type == MSG_PING) {
            if (send_message(c, MSG_PONG, nullptr, 0) != 0) {
                std::cout << "Client disconnected\n";
                break;
            }
        } else if (msg.type == MSG_BYE) {
            (void)send_message(c, MSG_BYE, nullptr, 0);
            break;
        }
    }

    ::close(c);
    ::close(s);
    return 0;
}