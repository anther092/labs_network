// client.cpp
// ЛР6: клиент с MessageEx, ACK/retry, /ping N, /netdiag,
// /list, /history, /w, /help.
//
// Запуск:
//   ./client 127.0.0.1 5000

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32

static const int ACK_TIMEOUT_MS = 2000;
static const int MAX_RETRIES = 3;
static const char* OFFLINE_MARKER = "__OFFLINE__|";

struct MessageEx {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    int64_t  timestamp;
    char     payload[MAX_PAYLOAD];
};

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
    MSG_HELP         = 14,

    MSG_ACK          = 15
};

struct PendingMsg {
    MessageEx msg;
    int64_t send_time_ms;
    int retries;
};

struct PingInfo {
    uint32_t msg_id;
    int number;
    int64_t send_time_ms;
    bool got_pong;
    double rtt_ms;
};

struct NetDiag {
    int sent = 0;
    int received = 0;
    double rtt_avg = 0.0;
    double jitter_avg = 0.0;
    double loss = 0.0;
};

static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ping_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_sock = -1;
static bool g_stop = false;
static std::string g_nickname;
static uint32_t g_local_msg_id = 1;

static std::vector<PendingMsg> g_pending;
static std::vector<PingInfo> g_pings;
static NetDiag g_last_diag;

// ------------------------ helpers ------------------------

static uint64_t host_to_net64(uint64_t value) {
    static const int test = 1;
    if (*(const char*)&test == 1) {
        uint32_t high = htonl((uint32_t)(value >> 32));
        uint32_t low  = htonl((uint32_t)(value & 0xFFFFFFFFULL));
        return ((uint64_t)low << 32) | high;
    }
    return value;
}

static uint64_t net_to_host64(uint64_t value) {
    return host_to_net64(value);
}

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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

static std::string trim_copy(const std::string& s) {
    size_t l = 0;
    size_t r = s.size();

    while (l < r && std::isspace((unsigned char)s[l])) ++l;
    while (r > l && std::isspace((unsigned char)s[r - 1])) --r;

    return s.substr(l, r - l);
}

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static std::string msg_type_name(uint8_t type) {
    switch (type) {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP: return "MSG_HELP";
        case MSG_ACK: return "MSG_ACK";
        default: return "MSG_UNKNOWN";
    }
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

// ------------------------ MessageEx ------------------------

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
    uint64_t net_ts = host_to_net64((uint64_t)msg.timestamp);

    std::vector<char> buf;
    buf.resize(4 + 1 + 4 + MAX_NAME + MAX_NAME + 8 + payload_len);

    size_t off = 0;

    std::memcpy(buf.data() + off, &net_length, 4);
    off += 4;

    std::memcpy(buf.data() + off, &msg.type, 1);
    off += 1;

    std::memcpy(buf.data() + off, &net_msg_id, 4);
    off += 4;

    std::memcpy(buf.data() + off, msg.sender, MAX_NAME);
    off += MAX_NAME;

    std::memcpy(buf.data() + off, msg.receiver, MAX_NAME);
    off += MAX_NAME;

    std::memcpy(buf.data() + off, &net_ts, 8);
    off += 8;

    if (payload_len > 0) {
        std::memcpy(buf.data() + off, msg.payload, payload_len);
    }

    return buf;
}

static int send_message_ex_raw(int fd, const MessageEx& msg) {
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

    msg.timestamp = (int64_t)net_to_host64(net_ts);
    std::memcpy(msg.payload, payload, payload_len + 1);

    return 1;
}

// ------------------------ pending ACK / retry ------------------------

static bool message_needs_ack(uint8_t type) {
    return type == MSG_TEXT || type == MSG_PRIVATE || type == MSG_PING;
}

static void add_pending(const MessageEx& msg) {
    if (!message_needs_ack(msg.type)) return;

    PendingMsg p{};
    p.msg = msg;
    p.send_time_ms = now_ms();
    p.retries = 0;

    pthread_mutex_lock(&g_pending_mutex);
    g_pending.push_back(p);
    pthread_mutex_unlock(&g_pending_mutex);
}

static void remove_pending_by_ack(uint32_t msg_id) {
    pthread_mutex_lock(&g_pending_mutex);

    for (size_t i = 0; i < g_pending.size(); ++i) {
        if (g_pending[i].msg.msg_id == msg_id) {
            std::cout << "[Transport][RETRY] ACK received (id=" << msg_id << ")\n";
            g_pending.erase(g_pending.begin() + (long)i);
            break;
        }
    }

    pthread_mutex_unlock(&g_pending_mutex);
}

static void send_reliable(const MessageEx& msg) {
    add_pending(msg);

    std::cout << "[Transport][RETRY] send "
              << msg_type_name(msg.type)
              << " (id=" << msg.msg_id << ")\n";

    if (send_message_ex_raw(g_sock, msg) != 0) {
        std::cout << "[Transport][RETRY] initial send failed (id="
                  << msg.msg_id << ")\n";
    }
}

static void* retry_thread_main(void*) {
    while (true) {
        pthread_mutex_lock(&g_state_mutex);
        bool stop = g_stop;
        int sock = g_sock;
        pthread_mutex_unlock(&g_state_mutex);

        if (stop) break;

        std::vector<MessageEx> to_resend;

        int64_t now = now_ms();

        pthread_mutex_lock(&g_pending_mutex);

        for (size_t i = 0; i < g_pending.size();) {
            PendingMsg& p = g_pending[i];

            if (now - p.send_time_ms >= ACK_TIMEOUT_MS) {
                std::cout << "[Transport][RETRY] wait ACK timeout (id="
                          << p.msg.msg_id << ")\n";

                if (p.retries >= MAX_RETRIES) {
                    std::cout << "[Transport][RETRY] delivery failed after "
                              << MAX_RETRIES
                              << " retries (id=" << p.msg.msg_id << ")\n";

                    g_pending.erase(g_pending.begin() + (long)i);
                    continue;
                }

                p.retries++;
                p.send_time_ms = now;

                std::cout << "[Transport][RETRY] resend "
                          << p.retries << "/" << MAX_RETRIES
                          << " (id=" << p.msg.msg_id << ")\n";

                to_resend.push_back(p.msg);
            }

            ++i;
        }

        pthread_mutex_unlock(&g_pending_mutex);

        for (const auto& msg : to_resend) {
            (void)send_message_ex_raw(sock, msg);
        }

        usleep(100 * 1000);
    }

    return nullptr;
}

// ------------------------ ping diagnostics ------------------------

static void add_ping(uint32_t id, int number, int64_t send_time) {
    PingInfo p{};
    p.msg_id = id;
    p.number = number;
    p.send_time_ms = send_time;
    p.got_pong = false;
    p.rtt_ms = 0.0;

    pthread_mutex_lock(&g_ping_mutex);
    g_pings.push_back(p);
    pthread_mutex_unlock(&g_ping_mutex);
}

static bool get_ping_result(uint32_t id, double& rtt_out) {
    bool found = false;

    pthread_mutex_lock(&g_ping_mutex);
    for (const auto& p : g_pings) {
        if (p.msg_id == id && p.got_pong) {
            rtt_out = p.rtt_ms;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_ping_mutex);

    return found;
}

static void mark_pong_received(uint32_t id) {
    int64_t now = now_ms();

    pthread_mutex_lock(&g_ping_mutex);

    for (auto& p : g_pings) {
        if (p.msg_id == id && !p.got_pong) {
            p.got_pong = true;
            p.rtt_ms = (double)(now - p.send_time_ms);
            break;
        }
    }

    pthread_mutex_unlock(&g_ping_mutex);
}

static void clear_ping_storage() {
    pthread_mutex_lock(&g_ping_mutex);
    g_pings.clear();
    pthread_mutex_unlock(&g_ping_mutex);
}

static void run_ping_series(int count) {
    if (count <= 0) count = 10;

    clear_ping_storage();

    std::vector<double> rtts;
    std::vector<double> jitters;

    double previous_rtt = -1.0;
    int received = 0;

    for (int i = 1; i <= count; ++i) {
        uint32_t id = next_local_msg_id();
        int64_t send_time = now_ms();

        add_ping(id, i, send_time);

        MessageEx ping = make_message(
            MSG_PING,
            id,
            g_nickname,
            "",
            std::time(nullptr),
            "PING"
        );

        send_reliable(ping);

        bool ok = false;
        double rtt = 0.0;

        int64_t start = now_ms();
        while (now_ms() - start < ACK_TIMEOUT_MS) {
            if (get_ping_result(id, rtt)) {
                ok = true;
                break;
            }
            usleep(20 * 1000);
        }

        if (!ok) {
            std::cout << "PING " << i << " -> timeout\n";
            continue;
        }

        received++;
        rtts.push_back(rtt);

        std::cout << std::fixed << std::setprecision(1);
        std::cout << "PING " << i << " -> RTT=" << rtt << "ms";

        if (previous_rtt >= 0.0) {
            double jitter = std::fabs(rtt - previous_rtt);
            jitters.push_back(jitter);
            std::cout << " | Jitter=" << jitter << "ms";
        }

        std::cout << "\n";
        previous_rtt = rtt;
    }

    double rtt_sum = 0.0;
    for (double v : rtts) rtt_sum += v;

    double jitter_sum = 0.0;
    for (double v : jitters) jitter_sum += v;

    g_last_diag.sent = count;
    g_last_diag.received = received;
    g_last_diag.rtt_avg = rtts.empty() ? 0.0 : rtt_sum / rtts.size();
    g_last_diag.jitter_avg = jitters.empty() ? 0.0 : jitter_sum / jitters.size();
    g_last_diag.loss = count == 0 ? 0.0 : ((double)(count - received) / (double)count) * 100.0;
}

static std::string json_escape(const std::string& s) {
    std::string out;

    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }

    return out;
}

static void save_netdiag_json() {
    std::string filename = "net_diag_" + g_nickname + ".json";
    std::ofstream out(filename, std::ios::trunc);

    if (!out) {
        std::cout << "[ERROR]: cannot write " << filename << "\n";
        return;
    }

    out << "{\n";
    out << "  \"nickname\": \"" << json_escape(g_nickname) << "\",\n";
    out << "  \"sent\": " << g_last_diag.sent << ",\n";
    out << "  \"received\": " << g_last_diag.received << ",\n";
    out << "  \"rtt_avg_ms\": " << g_last_diag.rtt_avg << ",\n";
    out << "  \"jitter_avg_ms\": " << g_last_diag.jitter_avg << ",\n";
    out << "  \"loss_percent\": " << g_last_diag.loss << "\n";
    out << "}\n";

    std::cout << "[CLIENT]: saved " << filename << "\n";
}

static void print_netdiag() {
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "RTT avg : " << g_last_diag.rtt_avg << " ms\n";
    std::cout << "Jitter  : " << g_last_diag.jitter_avg << " ms\n";
    std::cout << "Loss    : " << g_last_diag.loss << " %\n";

    save_netdiag_json();
}

// ------------------------ display ------------------------

static void print_help() {
    std::cout
        << "Available commands:\n"
        << "/help\n"
        << "/list\n"
        << "/history\n"
        << "/history N\n"
        << "/netdiag\n"
        << "/quit\n"
        << "/w <nick> <message>\n"
        << "/ping\n"
        << "/ping N\n"
        << "Tip: packets never sleep\n";
}

static void display_text(const MessageEx& msg) {
    std::cout << "[" << time_to_string(msg.timestamp) << "]"
              << "[id=" << msg.msg_id << "]"
              << "[" << msg.sender << "]: "
              << msg.payload << "\n";
}

static void display_private(const MessageEx& msg) {
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

// ------------------------ receiver ------------------------

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
            case MSG_ACK:
                remove_pending_by_ack(msg.msg_id);
                break;

            case MSG_PONG:
                mark_pong_received(msg.msg_id);
                break;

            case MSG_TEXT:
                display_text(msg);
                break;

            case MSG_PRIVATE:
                display_private(msg);
                break;

            case MSG_SERVER_INFO:
                std::cout << "[SERVER]: " << msg.payload << "\n";
                break;

            case MSG_HISTORY_DATA:
                std::cout << msg.payload << "\n";
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

static int send_normal(const MessageEx& msg) {
    return send_message_ex_raw(g_sock, msg);
}

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

    if (send_normal(hello) != 0) {
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

    if (send_normal(auth) != 0) {
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
        std::cerr << "pthread_create receiver failed\n";
        close(sock);
        return 1;
    }
    pthread_detach(recv_thread);

    pthread_t retry_thread;
    if (pthread_create(&retry_thread, nullptr, retry_thread_main, nullptr) != 0) {
        std::cerr << "pthread_create retry failed\n";
        close(sock);
        return 1;
    }
    pthread_detach(retry_thread);

    while (true) {
        pthread_mutex_lock(&g_state_mutex);
        bool stop = g_stop;
        pthread_mutex_unlock(&g_state_mutex);

        if (stop) break;

        std::cout << "> " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }

        line = trim_copy(line);
        if (line.empty()) continue;

        if (line == "/help") {
            print_help();
            continue;
        }

        if (line == "/netdiag") {
            print_netdiag();
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

            (void)send_normal(bye);
            break;
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

            (void)send_normal(list_msg);
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

            (void)send_normal(hist);
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

            (void)send_normal(hist);
            continue;
        }

        if (line == "/ping") {
            run_ping_series(10);
            continue;
        }

        if (starts_with(line, "/ping ")) {
            std::string arg = trim_copy(line.substr(6));
            int n = 10;

            try {
                n = std::stoi(arg);
                if (n <= 0) n = 10;
            } catch (...) {
                std::cout << "Usage: /ping N\n";
                continue;
            }

            run_ping_series(n);
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

            send_reliable(pm);
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

        send_reliable(text_msg);
    }

    pthread_mutex_lock(&g_state_mutex);
    g_stop = true;
    pthread_mutex_unlock(&g_state_mutex);

    close(sock);
    return 0;
}