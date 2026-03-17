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
    uint8_t type;
    char payload[MAX_PAYLOAD];
};

enum {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

// ---------- helpers ----------
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

static int send_message_str(int fd, uint8_t type, const std::string& s) {
    return send_message(fd, type, s.data(), (uint32_t)s.size());
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

// ---------- shared state ----------
static pthread_mutex_t g_state_mtx = PTHREAD_MUTEX_INITIALIZER;

static int g_sock = -1;
static bool g_connected = false;
static bool g_stop = false;

static std::string g_ip;
static int g_port = 0;
static std::string g_nick;

static void set_disconnected_locked() {
    if (g_sock != -1) {
        ::close(g_sock);
        g_sock = -1;
    }
    g_connected = false;
}

static bool is_connected() {
    pthread_mutex_lock(&g_state_mtx);
    bool c = g_connected;
    pthread_mutex_unlock(&g_state_mtx);
    return c;
}

static int current_sock() {
    pthread_mutex_lock(&g_state_mtx);
    int s = g_sock;
    pthread_mutex_unlock(&g_state_mtx);
    return s;
}

static int connect_and_handshake_once() {
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

    // HELLO
    if (send_message_str(sock, MSG_HELLO, g_nick) != 0) {
        ::close(sock);
        return -1;
    }

    // WELCOME
    Message msg{};
    std::string payload;
    int rc = recv_message(sock, msg, payload);
    if (rc <= 0 || msg.type != MSG_WELCOME) {
        ::close(sock);
        return -1;
    }

    std::cout << payload << "\n";

    pthread_mutex_lock(&g_state_mtx);
    g_sock = sock;
    g_connected = true;
    pthread_mutex_unlock(&g_state_mtx);

    return 0;
}

// receiver thread: reads from socket, prints messages, marks disconnected on failure
static void* recv_thread_main(void*) {
    while (true) {
        pthread_mutex_lock(&g_state_mtx);
        bool stop = g_stop;
        int sock = g_sock;
        bool connected = g_connected;
        pthread_mutex_unlock(&g_state_mtx);

        if (stop) break;
        if (!connected || sock == -1) {
            ::usleep(100 * 1000); // 100ms
            continue;
        }

        Message msg{};
        std::string payload;
        int rc = recv_message(sock, msg, payload);
        if (rc <= 0) {
            pthread_mutex_lock(&g_state_mtx);
            set_disconnected_locked();
            pthread_mutex_unlock(&g_state_mtx);

            std::cout << "Disconnected. Reconnecting...\n";
            continue;
        }

        if (msg.type == MSG_TEXT) {
            std::cout << payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "PONG\n";
        } else if (msg.type == MSG_BYE) {
            pthread_mutex_lock(&g_state_mtx);
            set_disconnected_locked();
            pthread_mutex_unlock(&g_state_mtx);

            std::cout << "Disconnected. Reconnecting...\n";
        }
    }
    return nullptr;
}

static void ensure_connected_forever() {
    while (!g_stop) {
        if (is_connected()) return;
        // попытка переподключения каждые 2 сек
        if (connect_and_handshake_once() == 0) return;
        ::sleep(2);
    }
}

static void safe_send(uint8_t type, const std::string& payload) {
    pthread_mutex_lock(&g_state_mtx);
    int sock = g_sock;
    bool connected = g_connected;
    pthread_mutex_unlock(&g_state_mtx);

    if (!connected || sock == -1) return;

    int rc = (payload.empty())
        ? send_message(sock, type, nullptr, 0)
        : send_message_str(sock, type, payload);

    if (rc != 0) {
        pthread_mutex_lock(&g_state_mtx);
        set_disconnected_locked();
        pthread_mutex_unlock(&g_state_mtx);
        std::cout << "Disconnected. Reconnecting...\n";
    }
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port> <nickname>\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    g_ip = argv[1];
    g_port = std::stoi(argv[2]);
    g_nick = argv[3];

    if (g_port <= 0 || g_port > 65535) {
        std::cerr << "Invalid port\n";
        return 1;
    }
    if (g_nick.empty() || g_nick.size() > MAX_PAYLOAD) {
        std::cerr << "Invalid nickname length\n";
        return 1;
    }

    // start receiver thread
    pthread_t rth;
    if (pthread_create(&rth, nullptr, recv_thread_main, nullptr) != 0) {
        std::cerr << "pthread_create failed\n";
        return 1;
    }
    pthread_detach(rth);

    std::cout << "Connecting...\n";
    ensure_connected_forever();

    // main loop: stdin with timeout so reconnect happens even if user doesn't type
    while (true) {
        pthread_mutex_lock(&g_state_mtx);
        bool stop = g_stop;
        pthread_mutex_unlock(&g_state_mtx);
        if (stop) break;

        if (!is_connected()) {
            ensure_connected_forever();
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        timeval tv{};
        tv.tv_sec = 1; // 1 sec timeout (чтобы периодически проверять соединение)
        tv.tv_usec = 0;

        int sel = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (sel == 0) {
            // timeout
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                // EOF -> выход
                safe_send(MSG_BYE, "");
                break;
            }

            if (line == "/ping") {
                safe_send(MSG_PING, "");
            } else if (line == "/quit") {
                safe_send(MSG_BYE, "");
                break;
            } else {
                if (line.size() > MAX_PAYLOAD) {
                    std::cerr << "Message too long (max " << MAX_PAYLOAD << ")\n";
                    continue;
                }
                safe_send(MSG_TEXT, line);
            }
        }
    }

    pthread_mutex_lock(&g_state_mtx);
    g_stop = true;
    set_disconnected_locked();
    pthread_mutex_unlock(&g_state_mtx);

    return 0;
}