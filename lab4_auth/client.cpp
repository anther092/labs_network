// commands:
//   /w <nick> <message> -> MSG_PRIVATE payload "target:message"
//   /ping               -> MSG_PING
//   /quit               -> MSG_BYE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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
    MSG_HELLO       = 1,
    MSG_WELCOME     = 2,
    MSG_TEXT        = 3,
    MSG_PING        = 4,
    MSG_PONG        = 5,
    MSG_BYE         = 6,
    MSG_AUTH        = 7,
    MSG_PRIVATE     = 8,
    MSG_ERROR       = 9,
    MSG_SERVER_INFO = 10
};

static ssize_t recv_all(int fd, void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::recv(fd, (char*)buf + off, n - off, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static ssize_t send_all(int fd, const void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t s = ::send(fd, (const char*)buf + off, n - off, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)s;
    }
    return (ssize_t)off;
}

static int send_message(int fd, uint8_t type, const std::string& payload) {
    uint32_t payload_len = (uint32_t)payload.size();
    if (payload_len > MAX_PAYLOAD) return -1;

    uint32_t length = 1u + payload_len;
    uint32_t net_len = htonl(length);

    std::string buf;
    buf.resize(sizeof(net_len) + sizeof(type) + payload_len);

    std::memcpy(&buf[0], &net_len, sizeof(net_len));
    std::memcpy(&buf[sizeof(net_len)], &type, sizeof(type));
    if (payload_len > 0) std::memcpy(&buf[sizeof(net_len) + sizeof(type)], payload.data(), payload_len);

    if (send_all(fd, buf.data(), buf.size()) != (ssize_t)buf.size()) return -1;
    return 0;
}

static int send_message_empty(int fd, uint8_t type) {
    return send_message(fd, type, "");
}

static int recv_message(int fd, Message& msg, std::string& payload_out) {
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
    payload_out.assign(msg.payload, msg.payload + payload_len);
    return 1;
}

// ---- shared state between threads ----
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_sock = -1;
static bool g_connected = false;
static bool g_stop = false;

static std::string g_ip;
static int g_port = 0;
static std::string g_nick;

static void set_disconnected_locked() {
    if (g_sock != -1) { ::close(g_sock); g_sock = -1; }
    g_connected = false;
}

static int connect_and_handshake() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons((uint16_t)g_port);
    if (::inet_pton(AF_INET, g_ip.c_str(), &srv.sin_addr) != 1) {
        ::close(sock);
        return -1;
    }

    if (::connect(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        ::close(sock);
        return -1;
    }

    // pre-sync HELLO/WELCOME (unchanged)
    if (send_message(sock, MSG_HELLO, "HELLO") != 0) { ::close(sock); return -1; }

    Message msg{};
    std::string payload;
    int rc = recv_message(sock, msg, payload);
    if (rc <= 0 || msg.type != MSG_WELCOME) { ::close(sock); return -1; }
    std::cout << payload << "\n";

    // AUTH
    if (send_message(sock, MSG_AUTH, g_nick) != 0) { ::close(sock); return -1; }

    pthread_mutex_lock(&g_mtx);
    g_sock = sock;
    g_connected = true;
    pthread_mutex_unlock(&g_mtx);

    return 0;
}

static void ensure_connected() {
    while (true) {
        pthread_mutex_lock(&g_mtx);
        bool stop = g_stop;
        bool ok = g_connected;
        pthread_mutex_unlock(&g_mtx);
        if (stop) return;
        if (ok) return;

        std::cout << "Connecting...\n";
        if (connect_and_handshake() == 0) return;
        std::cout << "Disconnected. Reconnecting...\n";
        ::sleep(2);
    }
}

static void safe_send(uint8_t type, const std::string& payload) {
    pthread_mutex_lock(&g_mtx);
    int sock = g_sock;
    bool ok = g_connected;
    pthread_mutex_unlock(&g_mtx);

    if (!ok || sock == -1) return;

    int rc = payload.empty() ? send_message_empty(sock, type) : send_message(sock, type, payload);
    if (rc != 0) {
        pthread_mutex_lock(&g_mtx);
        set_disconnected_locked();
        pthread_mutex_unlock(&g_mtx);
        std::cout << "Disconnected. Reconnecting...\n";
    }
}

static void* recv_thread(void*) {
    while (true) {
        pthread_mutex_lock(&g_mtx);
        bool stop = g_stop;
        int sock = g_sock;
        bool ok = g_connected;
        pthread_mutex_unlock(&g_mtx);

        if (stop) break;
        if (!ok || sock == -1) { ::usleep(100 * 1000); continue; }

        Message msg{};
        std::string payload;
        int rc = recv_message(sock, msg, payload);
        if (rc <= 0) {
            pthread_mutex_lock(&g_mtx);
            set_disconnected_locked();
            pthread_mutex_unlock(&g_mtx);
            std::cout << "Disconnected. Reconnecting...\n";
            continue;
        }

        switch (msg.type) {
            case MSG_TEXT:
                std::cout << payload << "\n";
                break;
            case MSG_PRIVATE:
                std::cout << payload << "\n";
                break;
            case MSG_SERVER_INFO:
                std::cout << "[SERVER]: " << payload << "\n";
                break;
            case MSG_PONG:
                std::cout << "PONG\n";
                break;
            case MSG_ERROR:
                std::cout << "[ERROR]: " << payload << "\n";
                // для ошибки аутентификации обычно стоит завершиться
                pthread_mutex_lock(&g_mtx);
                g_stop = true;
                set_disconnected_locked();
                pthread_mutex_unlock(&g_mtx);
                break;
            case MSG_BYE:
                pthread_mutex_lock(&g_mtx);
                set_disconnected_locked();
                pthread_mutex_unlock(&g_mtx);
                std::cout << "Disconnected. Reconnecting...\n";
                break;
            default:
                // ignore
                break;
        }
    }
    return nullptr;
}

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port>\n";
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    g_ip = argv[1];
    g_port = std::stoi(argv[2]);
    if (g_port <= 0 || g_port > 65535) {
        std::cerr << "Invalid port\n";
        return 1;
    }

    std::cout << "Enter nickname: ";
    std::getline(std::cin, g_nick);
    if (g_nick.empty()) {
        std::cerr << "Nickname is required\n";
        return 1;
    }

    pthread_t th;
    if (pthread_create(&th, nullptr, recv_thread, nullptr) != 0) {
        std::cerr << "pthread_create failed\n";
        return 1;
    }
    pthread_detach(th);

    ensure_connected();

    // main input loop (with timeout so reconnect can happen even if user is silent)
    while (true) {
        pthread_mutex_lock(&g_mtx);
        bool stop = g_stop;
        pthread_mutex_unlock(&g_mtx);
        if (stop) break;

        if (!g_connected) {
            ensure_connected();
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                safe_send(MSG_BYE, "");
                break;
            }

            if (line == "/quit") {
                safe_send(MSG_BYE, "");
                break;
            } else if (line == "/ping") {
                safe_send(MSG_PING, "");
            } else if (starts_with(line, "/w ")) {
                // /w <nick> <message>
                std::string rest = line.substr(3);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }
                std::string target = rest.substr(0, sp);
                std::string msg = rest.substr(sp + 1);
                if (target.empty() || msg.empty()) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }
                safe_send(MSG_PRIVATE, target + ":" + msg);
            } else {
                if ((int)line.size() > MAX_PAYLOAD) {
                    std::cout << "Message too long (max " << MAX_PAYLOAD << ")\n";
                    continue;
                }
                safe_send(MSG_TEXT, line);
            }
        }
    }

    pthread_mutex_lock(&g_mtx);
    g_stop = true;
    set_disconnected_locked();
    pthread_mutex_unlock(&g_mtx);

    return 0;
}