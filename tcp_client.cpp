#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <sys/select.h>
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

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port> <nickname>\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    const std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    const std::string nick = argv[3];

    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port.\n";
        return 1;
    }
    if (nick.empty() || nick.size() > MAX_PAYLOAD) {
        std::cerr << "Invalid nickname length.\n";
        return 1;
    }

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &srv.sin_addr) != 1) {
        std::cerr << "Invalid server IP.\n";
        close(sock);
        return 1;
    }

    if (connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    std::cout << "Connected\n";

    if (send_message(sock, MSG_HELLO, nick.c_str(), (uint32_t)nick.size()) != 0) {
        std::cerr << "Failed to send HELLO\n";
        ::close(sock);
        return 1;
    }

    Message msg{};
    int rc = recv_message(sock, msg);
    if (rc <= 0 || msg.type != MSG_WELCOME) {
        std::cerr << "Disconnected before WELCOME\n";
        ::close(sock);
        return 1;
    }
    std::cout << msg.payload << "\n";

    while (true) {
        std::cout << "> " << std::flush;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sock, &rfds);

        int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
        int sel = select(maxfd + 1, &rfds, nullptr, nullptr, nullptr);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(sock, &rfds)) {
            rc = recv_message(sock, msg);
            if (rc == 0) {
                std::cout << "\nDisconnected\n";
                break;
            }
            if (rc < 0) {
                std::cout << "\nDisconnected\n";
                break;
            }

            if (msg.type == MSG_TEXT) {
                std::cout << "\n" << msg.payload << "\n";
            } else if (msg.type == MSG_PONG) {
                std::cout << "\nPONG\n";
            } else if (msg.type == MSG_BYE) {
                std::cout << "\nDisconnected\n";
                break;
            } else if (msg.type == MSG_WELCOME) {
                std::cout << "\n" << msg.payload << "\n";
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) { // EOF
                (void)send_message(sock, MSG_BYE, nullptr, 0);
                std::cout << "\nDisconnected\n";
                break;
            }

            if (line == "/ping") {
                if (send_message(sock, MSG_PING, nullptr, 0) != 0) break;
            } else if (line == "/quit") {
                (void)send_message(sock, MSG_BYE, nullptr, 0);
                std::cout << "Disconnected\n";
                break;
            } else {
                if (line.size() > MAX_PAYLOAD) {
                    std::cerr << "Message too long (max " << MAX_PAYLOAD << ")\n";
                    continue;
                }
                if (send_message(sock, MSG_TEXT, line.c_str(), (uint32_t)line.size()) != 0) break;
            }
        }
    }

    ::close(sock);
    return 0;
}