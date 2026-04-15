// client.cpp
// ЛР5: клиент для расширенного TCP-чата с MessageEx.
// macOS / Linux
//
// Сборка:
//   clang++ -std=c++17 -Wall -Wextra -O2 -pthread client.cpp -o client
// или
//   g++ -std=c++17 -Wall -Wextra -O2 -pthread client.cpp -o client

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32

static const char* OFFLINE_MARKER = "__OFFLINE__|";

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    int64_t  timestamp;
    char     payload[MAX_PAYLOAD];
} MessageEx;

enum {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,
    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,
    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14
};

static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_sock = -1;
static bool g_stop = false;
static std::string g_nickname;
static uint32_t g_local_msg_id = 1;

// ------------------------ Helpers ------------------------

static uint64_t htonll(uint64_t value) {
    static const int test = 1;
    if (*(const char*)&test == 1) {
        uint32_t high = htonl((uint32_t)(value >> 32));
        uint32_t low  = htonl((uint32_t)(value & 0xFFFFFFFFULL));
        return ((uint64_t)low << 32) | high;
    }
    return value;
}

static uint64_t ntohll(uint64_t value) {
    return htonll(value);
}

static std::string time_to_string(int64_t ts) {
    char buf[MAX_TIME_STR];
    std::time_t t = (std::time_t)ts;
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

static uint32_t next_local_msg_id() {
    return g_local_msg_id++;
}

static ssize_t recv_all(int fd, void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, static_cast<char*>(buf) + off, n - off, 0);
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
        ssize_t s = send(fd, static_cast<const char*>(buf) + off, n - off, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)s;
    }
    return (ssize_t)off;
}

static MessageEx make_message(uint8_t type,
                              uint32_t msg_id,
                              const std::string& sender,
                              const std::string& receiver,
                              int64_t timestamp,
                              const std::string& payload) {
    MessageEx msg{};
    msg.length = (uint32_t)payload.size();
    msg.type = type;
    msg.msg_id = msg_id;
    std::snprintf(msg.sender, sizeof(msg.sender), "%s", sender.c_str());
    std::snprintf(msg.receiver, sizeof(msg.receiver), "%s", receiver.c_str());
    msg.timestamp = timestamp;
    std::snprintf(msg.payload, sizeof(msg.payload), "%s", payload.c_str());
    return msg;
}

static std::vector<char> serialize_message(const MessageEx& msg) {
    uint32_t payload_len = (uint32_t)std::strlen(msg.payload);

    uint32_t net_length = htonl(payload_len);
    uint32_t net_msg_id = htonl(msg.msg_id);
    uint64_t net_ts = htonll((uint64_t)msg.timestamp);

    std::vector<char> buf;
    buf.resize(4 + 1 + 4 + MAX_NAME + MAX_NAME + 8 + payload_len);

    size_t off = 0;
    std::memcpy(buf.data() + off, &net_length, 4); off += 4;
    std::memcpy(buf.data() + off, &msg.type, 1); off += 1;
    std::memcpy(buf.data() + off, &net_msg_id, 4); off += 4;
    std::memcpy(buf.data() + off, msg.sender, MAX_NAME); off += MAX_NAME;
    std::memcpy(buf.data() + off, msg.receiver, MAX_NAME); off += MAX_NAME;
    std::memcpy(buf.data() + off, &net_ts, 8); off += 8;
    if (payload_len > 0) {
        std::memcpy(buf.data() + off, msg.payload, payload_len);
    }

    return buf;
}

static int send_message_ex(int fd, const MessageEx& msg) {
    std::vector<char> buf = serialize_message(msg);

    pthread_mutex_lock(&g_send_mutex);
    int ok = (send_all(fd, buf.data(), buf.size()) == (ssize_t)buf.size()) ? 0 : -1;
    pthread_mutex_unlock(&g_send_mutex);

    return ok;
}

static int recv_message_ex(int fd, MessageEx& msg) {
    uint32_t net_length = 0;
    uint8_t type = 0;
    uint32_t net_msg_id = 0;
    char sender[MAX_NAME] = {0};
    char receiver[MAX_NAME] = {0};
    uint64_t net_ts = 0;

    ssize_t r = recv_all(fd, &net_length, 4);
    if (r == 0) return 0;
    if (r < 0) return -1;

    r = recv_all(fd, &type, 1);
    if (r == 0) return 0;
    if (r < 0) return -1;

    r = recv_all(fd, &net_msg_id, 4);
    if (r == 0) return 0;
    if (r < 0) return -1;

    r = recv_all(fd, sender, MAX_NAME);
    if (r == 0) return 0;
    if (r < 0) return -1;

    r = recv_all(fd, receiver, MAX_NAME);
    if (r == 0) return 0;
    if (r < 0) return -1;

    r = recv_all(fd, &net_ts, 8);
    if (r == 0) return 0;
    if (r < 0) return -1;

    uint32_t payload_len = ntohl(net_length);
    if (payload_len >= MAX_PAYLOAD) return -2;

    char payload[MAX_PAYLOAD] = {0};
    if (payload_len > 0) {
        r = recv_all(fd, payload, payload_len);
        if (r == 0) return 0;
        if (r < 0) return -1;
    }
    payload[payload_len] = '\0';

    msg.length = payload_len;
    msg.type = type;
    msg.msg_id = ntohl(net_msg_id);
    std::memcpy(msg.sender, sender, MAX_NAME);
    msg.sender[MAX_NAME - 1] = '\0';
    std::memcpy(msg.receiver, receiver, MAX_NAME);
    msg.receiver[MAX_NAME - 1] = '\0';
    msg.timestamp = (int64_t)ntohll(net_ts);
    std::memcpy(msg.payload, payload, payload_len + 1);

    return 1;
}

static std::string trim_copy(const std::string& s) {
    size_t left = 0;
    size_t right = s.size();

    while (left < right && std::isspace((unsigned char)s[left])) ++left;
    while (right > left && std::isspace((unsigned char)s[right - 1])) --right;

    return s.substr(left, right - left);
}

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// ------------------------ Display helpers ------------------------

static void print_help() {
    std::cout
        << "/help\n"
        << "/list\n"
        << "/history\n"
        << "/history N\n"
        << "/quit\n"
        << "/w <nick> <message>\n"
        << "/ping\n"
        << "Tip: packets never sleep\n";
}

static void display_text_message(const MessageEx& msg) {
    std::cout << "[" << time_to_string(msg.timestamp) << "]"
              << "[id=" << msg.msg_id << "]"
              << "[" << msg.sender << "]: "
              << msg.payload << "\n";
}

static void display_private_message(const MessageEx& msg) {
    std::string text = msg.payload;

    if (starts_with(text, OFFLINE_MARKER)) {
        text = text.substr(std::strlen(OFFLINE_MARKER));
        std::cout << "[" << time_to_string(msg.timestamp) << "]"
                  << "[id=" << msg.msg_id << "]"
                  << "[OFFLINE]"
                  << "[" << msg.sender << " -> " << msg.receiver << "]: "
                  << text << "\n";
    } else {
        std::cout << "[" << time_to_string(msg.timestamp) << "]"
                  << "[id=" << msg.msg_id << "]"
                  << "[PRIVATE]"
                  << "[" << msg.sender << " -> " << msg.receiver << "]: "
                  << text << "\n";
    }
}

// ------------------------ Receiver thread ------------------------

static void* receiver_thread_main(void*) {
    while (true) {
        pthread_mutex_lock(&g_state_mutex);
        bool stop = g_stop;
        int sock = g_sock;
        pthread_mutex_unlock(&g_state_mutex);

        if (stop) break;

        MessageEx msg{};
        int rc = recv_message_ex(sock, msg);
        if (rc <= 0) {
            std::cout << "Disconnected\n";
            pthread_mutex_lock(&g_state_mutex);
            g_stop = true;
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }

        switch (msg.type) {
            case MSG_TEXT:
                display_text_message(msg);
                break;

            case MSG_PRIVATE:
                display_private_message(msg);
                break;

            case MSG_SERVER_INFO:
                std::cout << "[SERVER]: " << msg.payload << "\n";
                break;

            case MSG_HISTORY_DATA:
                std::cout << msg.payload << "\n";
                break;

            case MSG_PONG:
                std::cout << "[SERVER]: PONG\n";
                break;

            case MSG_ERROR:
                std::cout << "[ERROR]: " << msg.payload << "\n";
                break;

            case MSG_BYE:
                std::cout << "Disconnected\n";
                pthread_mutex_lock(&g_state_mutex);
                g_stop = true;
                pthread_mutex_unlock(&g_state_mutex);
                return nullptr;

            default:
                break;
        }
    }

    return nullptr;
}

// ------------------------ main ------------------------

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port>\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    std::string server_ip = argv[1];
    int port = std::stoi(argv[2]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port\n";
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "Invalid server IP\n";
        close(sock);
        return 1;
    }

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    pthread_mutex_lock(&g_state_mutex);
    g_sock = sock;
    pthread_mutex_unlock(&g_state_mutex);

    std::cout << "Connected\n";

    // HELLO
    MessageEx hello = make_message(
        MSG_HELLO,
        next_local_msg_id(),
        "",
        "",
        std::time(nullptr),
        "HELLO"
    );

    if (send_message_ex(sock, hello) != 0) {
        std::cerr << "Failed to send HELLO\n";
        close(sock);
        return 1;
    }

    // WELCOME
    MessageEx msg{};
    int rc = recv_message_ex(sock, msg);
    if (rc <= 0 || msg.type != MSG_WELCOME) {
        std::cerr << "Failed to receive WELCOME\n";
        close(sock);
        return 1;
    }

    std::cout << msg.payload << "\n";

    // AUTH
    std::cout << "Enter nickname: ";
    std::getline(std::cin, g_nickname);
    g_nickname = trim_copy(g_nickname);

    if (g_nickname.empty()) {
        std::cerr << "Nickname is required\n";
        close(sock);
        return 1;
    }

    MessageEx auth = make_message(
        MSG_AUTH,
        next_local_msg_id(),
        g_nickname,
        "",
        std::time(nullptr),
        g_nickname
    );

    if (send_message_ex(sock, auth) != 0) {
        std::cerr << "Failed to send AUTH\n";
        close(sock);
        return 1;
    }

    rc = recv_message_ex(sock, msg);
    if (rc <= 0) {
        std::cerr << "Authentication failed\n";
        close(sock);
        return 1;
    }

    if (msg.type == MSG_ERROR) {
        std::cout << "[ERROR]: " << msg.payload << "\n";
        close(sock);
        return 1;
    }

    if (msg.type == MSG_SERVER_INFO) {
        std::cout << "[SERVER]: " << msg.payload << "\n";
    }

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, nullptr, receiver_thread_main, nullptr) != 0) {
        std::cerr << "pthread_create failed\n";
        close(sock);
        return 1;
    }
    pthread_detach(recv_thread);

    while (true) {
        pthread_mutex_lock(&g_state_mutex);
        bool stop = g_stop;
        pthread_mutex_unlock(&g_state_mutex);
        if (stop) break;

        std::cout << "> " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            MessageEx bye = make_message(
                MSG_BYE,
                next_local_msg_id(),
                g_nickname,
                "",
                std::time(nullptr),
                "BYE"
            );
            (void)send_message_ex(sock, bye);
            break;
        }

        line = trim_copy(line);
        if (line.empty()) continue;

        if (line == "/help") {
            print_help();
            continue;
        }

        if (line == "/quit") {
            MessageEx bye = make_message(
                MSG_BYE,
                next_local_msg_id(),
                g_nickname,
                "",
                std::time(nullptr),
                "BYE"
            );
            (void)send_message_ex(sock, bye);
            break;
        }

        if (line == "/ping") {
            MessageEx ping = make_message(
                MSG_PING,
                next_local_msg_id(),
                g_nickname,
                "",
                std::time(nullptr),
                "PING"
            );
            (void)send_message_ex(sock, ping);
            continue;
        }

        if (line == "/list") {
            MessageEx list_msg = make_message(
                MSG_LIST,
                next_local_msg_id(),
                g_nickname,
                "",
                std::time(nullptr),
                ""
            );
            (void)send_message_ex(sock, list_msg);
            continue;
        }

        if (line == "/history") {
            MessageEx hist = make_message(
                MSG_HISTORY,
                next_local_msg_id(),
                g_nickname,
                "",
                std::time(nullptr),
                ""
            );
            (void)send_message_ex(sock, hist);
            continue;
        }

        if (starts_with(line, "/history ")) {
            std::string count = trim_copy(line.substr(9));
            MessageEx hist = make_message(
                MSG_HISTORY,
                next_local_msg_id(),
                g_nickname,
                "",
                std::time(nullptr),
                count
            );
            (void)send_message_ex(sock, hist);
            continue;
        }

        if (starts_with(line, "/w ")) {
            std::string rest = line.substr(3);
            size_t pos = rest.find(' ');
            if (pos == std::string::npos) {
                std::cout << "Usage: /w <nick> <message>\n";
                continue;
            }

            std::string target = trim_copy(rest.substr(0, pos));
            std::string text = rest.substr(pos + 1);

            if (target.empty() || text.empty()) {
                std::cout << "Usage: /w <nick> <message>\n";
                continue;
            }

            MessageEx pm = make_message(
                MSG_PRIVATE,
                next_local_msg_id(),
                g_nickname,
                target,
                std::time(nullptr),
                text
            );
            (void)send_message_ex(sock, pm);
            continue;
        }

        MessageEx text_msg = make_message(
            MSG_TEXT,
            next_local_msg_id(),
            g_nickname,
            "",
            std::time(nullptr),
            line
        );
        (void)send_message_ex(sock, text_msg);
    }

    pthread_mutex_lock(&g_state_mutex);
    g_stop = true;
    pthread_mutex_unlock(&g_state_mutex);

    close(sock);
    return 0;
}