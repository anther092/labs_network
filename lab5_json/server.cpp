// server.cpp
// ЛР5: расширенный TCP-чат с MessageEx, JSON history, offline store&forward,
// TCP/IP logging, thread pool на pthreads.
// macOS / Linux
//
// Сборка:
//   clang++ -std=c++17 -Wall -Wextra -O2 -pthread server.cpp -o server
// или
//   g++ -std=c++17 -Wall -Wextra -O2 -pthread server.cpp -o server

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

#include <deque>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define MAX_TIME_STR 32

static const int WORKER_COUNT = 10;
static const int DEFAULT_HISTORY_COUNT = 10;
static const char* HISTORY_FILE = "chat_history.json";
static const char* OFFLINE_MARKER = "__OFFLINE__|";

typedef struct {
    uint32_t length;                 // длина payload
    uint8_t  type;                   // тип сообщения
    uint32_t msg_id;                 // уникальный идентификатор сообщения
    char     sender[MAX_NAME];       // ник отправителя
    char     receiver[MAX_NAME];     // ник получателя или ""
    int64_t  timestamp;              // время создания (в секундах)
    char     payload[MAX_PAYLOAD];   // текст / данные команды
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

struct Client {
    int sock;
    std::string nickname;
    bool authenticated;
    std::string addr;
};

struct HistoryRecord {
    uint32_t msg_id;
    int64_t timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool delivered;
    bool is_offline;
};

// ------------------------ Глобальные данные ------------------------

static std::deque<int> g_jobs;
static pthread_mutex_t g_jobs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_jobs_cv    = PTHREAD_COND_INITIALIZER;

static std::vector<Client*> g_clients;
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static std::vector<HistoryRecord> g_history;
static pthread_mutex_t g_history_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_id_mutex   = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_next_msg_id = 1;

// ------------------------ Вспомогательные функции ------------------------

static void log_line(const std::string& s) {
    pthread_mutex_lock(&g_log_mutex);
    std::cout << s << std::endl;
    pthread_mutex_unlock(&g_log_mutex);
}

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

static std::string trim_copy(const std::string& s) {
    size_t left = 0;
    size_t right = s.size();

    while (left < right && std::isspace((unsigned char)s[left])) {
        ++left;
    }
    while (right > left && std::isspace((unsigned char)s[right - 1])) {
        --right;
    }
    return s.substr(left, right - left);
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
        default: return "MSG_UNKNOWN";
    }
}

static std::string time_to_string(int64_t ts) {
    char buf[MAX_TIME_STR];
    std::time_t t = (std::time_t)ts;
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

static uint32_t next_msg_id() {
    pthread_mutex_lock(&g_id_mutex);
    uint32_t id = g_next_msg_id++;
    pthread_mutex_unlock(&g_id_mutex);
    return id;
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

static std::string peer_address(int fd) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (sockaddr*)&peer, &len) != 0) return "unknown:0";

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
}

static void get_endpoints(int fd,
                          std::string& src_ip,
                          uint16_t& src_port,
                          std::string& dst_ip,
                          uint16_t& dst_port) {
    sockaddr_in peer{};
    sockaddr_in local{};
    socklen_t peer_len = sizeof(peer);
    socklen_t local_len = sizeof(local);

    src_ip = "0.0.0.0";
    dst_ip = "0.0.0.0";
    src_port = 0;
    dst_port = 0;

    if (getpeername(fd, (sockaddr*)&peer, &peer_len) == 0) {
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        src_ip = ip;
        src_port = ntohs(peer.sin_port);
    }

    if (getsockname(fd, (sockaddr*)&local, &local_len) == 0) {
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        dst_ip = ip;
        dst_port = ntohs(local.sin_port);
    }
}

// ------------------------ TCP/IP логирование ------------------------

static void tcpip_log_recv(int fd, size_t bytes, const MessageEx& msg) {
    std::string src_ip, dst_ip;
    uint16_t src_port = 0, dst_port = 0;
    get_endpoints(fd, src_ip, src_port, dst_ip, dst_port);

    log_line("[Network Access] frame received via network interface");
    {
        std::ostringstream oss;
        oss << "[Internet] src=" << src_ip << " dst=" << dst_ip << " proto=TCP";
        log_line(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "[Transport] recv() " << bytes << " bytes via TCP"
            << " src_port=" << src_port << " dst_port=" << dst_port;
        log_line(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "[Application] deserialize MessageEx -> " << msg_type_name(msg.type);
        if (std::strlen(msg.sender) > 0) {
            oss << " from " << msg.sender;
        }
        log_line(oss.str());
    }
}

static void tcpip_log_send(int fd, size_t bytes, const MessageEx& msg, const std::string& note) {
    std::string src_ip, dst_ip;
    uint16_t src_port = 0, dst_port = 0;
    get_endpoints(fd, src_ip, src_port, dst_ip, dst_port);

    {
        std::ostringstream oss;
        oss << "[Application] " << note;
        log_line(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "[Transport] send() " << bytes << " bytes via TCP"
            << " src_port=" << dst_port << " dst_port=" << src_port;
        log_line(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "[Internet] destination ip = " << src_ip;
        log_line(oss.str());
    }
    log_line("[Network Access] frame sent to network interface");
}

// ------------------------ JSON helpers ------------------------

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += ch;     break;
        }
    }
    return out;
}

static std::string json_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case '\\': out += '\\'; break;
                case '"':  out += '"';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += n;    break;
            }
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

static bool extract_string_field(const std::string& obj, const std::string& key, std::string& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    std::smatch m;
    if (!std::regex_search(obj, m, re)) return false;
    out = json_unescape(m[1].str());
    return true;
}

static bool extract_uint_field(const std::string& obj, const std::string& key, uint32_t& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch m;
    if (!std::regex_search(obj, m, re)) return false;
    out = (uint32_t)std::stoul(m[1].str());
    return true;
}

static bool extract_i64_field(const std::string& obj, const std::string& key, int64_t& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (!std::regex_search(obj, m, re)) return false;
    out = std::stoll(m[1].str());
    return true;
}

static bool extract_bool_field(const std::string& obj, const std::string& key, bool& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (!std::regex_search(obj, m, re)) return false;
    out = (m[1].str() == "true");
    return true;
}

static bool save_history_to_disk_unlocked() {
    std::ofstream out(HISTORY_FILE, std::ios::trunc);
    if (!out) return false;

    out << "[\n";
    for (size_t i = 0; i < g_history.size(); ++i) {
        const HistoryRecord& r = g_history[i];
        out << "  {\n";
        out << "    \"msg_id\": " << r.msg_id << ",\n";
        out << "    \"timestamp\": " << r.timestamp << ",\n";
        out << "    \"sender\": \"" << json_escape(r.sender) << "\",\n";
        out << "    \"receiver\": \"" << json_escape(r.receiver) << "\",\n";
        out << "    \"type\": \"" << json_escape(r.type) << "\",\n";
        out << "    \"text\": \"" << json_escape(r.text) << "\",\n";
        out << "    \"delivered\": " << (r.delivered ? "true" : "false") << ",\n";
        out << "    \"is_offline\": " << (r.is_offline ? "true" : "false") << "\n";
        out << "  }";
        if (i + 1 < g_history.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
    return true;
}

static bool load_history_from_disk_unlocked(std::vector<HistoryRecord>& out_vec) {
    out_vec.clear();

    std::ifstream in(HISTORY_FILE);
    if (!in) {
        return true;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string content = buffer.str();
    if (trim_copy(content).empty()) {
        return true;
    }

    std::regex obj_re("\\{[^\\{\\}]*\\}");
    auto begin = std::sregex_iterator(content.begin(), content.end(), obj_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string obj = it->str();
        HistoryRecord r{};
        if (!extract_uint_field(obj, "msg_id", r.msg_id)) continue;
        if (!extract_i64_field(obj, "timestamp", r.timestamp)) continue;
        if (!extract_string_field(obj, "sender", r.sender)) continue;
        if (!extract_string_field(obj, "receiver", r.receiver)) continue;
        if (!extract_string_field(obj, "type", r.type)) continue;
        if (!extract_string_field(obj, "text", r.text)) continue;
        if (!extract_bool_field(obj, "delivered", r.delivered)) continue;
        if (!extract_bool_field(obj, "is_offline", r.is_offline)) continue;
        out_vec.push_back(r);
    }

    return true;
}

static void append_history_record(const HistoryRecord& rec) {
    pthread_mutex_lock(&g_history_mutex);
    g_history.push_back(rec);
    save_history_to_disk_unlocked();
    pthread_mutex_unlock(&g_history_mutex);
}

static void mark_history_delivered(uint32_t msg_id) {
    pthread_mutex_lock(&g_history_mutex);
    for (auto& r : g_history) {
        if (r.msg_id == msg_id) {
            r.delivered = true;
            break;
        }
    }
    save_history_to_disk_unlocked();
    pthread_mutex_unlock(&g_history_mutex);
}

static std::vector<HistoryRecord> load_history_snapshot_from_file() {
    std::vector<HistoryRecord> snapshot;
    pthread_mutex_lock(&g_history_mutex);
    if (!load_history_from_disk_unlocked(snapshot)) {
        snapshot = g_history;
    }
    pthread_mutex_unlock(&g_history_mutex);
    return snapshot;
}

// ------------------------ Форматирование истории ------------------------

static std::string format_history_record(const HistoryRecord& r) {
    std::ostringstream oss;
    std::string ts = time_to_string(r.timestamp);

    if (r.type == "MSG_TEXT") {
        oss << "[" << ts << "][id=" << r.msg_id << "][" << r.sender << "]: " << r.text;
    } else if (r.type == "MSG_PRIVATE") {
        if (r.is_offline) {
            oss << "[" << ts << "][id=" << r.msg_id << "][OFFLINE]["
                << r.sender << " -> " << r.receiver << "]: " << r.text;
        } else {
            oss << "[" << ts << "][id=" << r.msg_id << "][PRIVATE]["
                << r.sender << " -> " << r.receiver << "]: " << r.text;
        }
    } else if (r.type == "MSG_SERVER_INFO") {
        oss << "[" << ts << "][id=" << r.msg_id << "][SERVER]: " << r.text;
    } else {
        oss << "[" << ts << "][id=" << r.msg_id << "][" << r.type << "]: " << r.text;
    }

    return oss.str();
}

// ------------------------ MessageEx helpers ------------------------

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
    uint64_t net_ts     = htonll((uint64_t)msg.timestamp);

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
    if (payload_len >= MAX_PAYLOAD) {
        return -2;
    }

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

    size_t total_bytes = 4 + 1 + 4 + MAX_NAME + MAX_NAME + 8 + payload_len;
    tcpip_log_recv(fd, total_bytes, msg);
    return 1;
}

static int send_message_ex(int fd, const MessageEx& msg, const std::string& app_note) {
    std::vector<char> buf = serialize_message(msg);
    tcpip_log_send(fd, buf.size(), msg, app_note);

    pthread_mutex_lock(&g_send_mutex);
    int ok = (send_all(fd, buf.data(), buf.size()) == (ssize_t)buf.size()) ? 0 : -1;
    pthread_mutex_unlock(&g_send_mutex);
    return ok;
}

// ------------------------ Очередь задач ------------------------

static void push_job(int fd) {
    pthread_mutex_lock(&g_jobs_mutex);
    g_jobs.push_back(fd);
    pthread_mutex_unlock(&g_jobs_mutex);
    pthread_cond_signal(&g_jobs_cv);
}

static int pop_job_blocking() {
    pthread_mutex_lock(&g_jobs_mutex);
    while (g_jobs.empty()) {
        pthread_cond_wait(&g_jobs_cv, &g_jobs_mutex);
    }
    int fd = g_jobs.front();
    g_jobs.pop_front();
    pthread_mutex_unlock(&g_jobs_mutex);
    return fd;
}

// ------------------------ Работа со списком клиентов ------------------------

static Client* find_client_by_fd_locked(int fd) {
    for (Client* c : g_clients) {
        if (c->sock == fd) return c;
    }
    return nullptr;
}

static Client* find_client_by_nick_locked(const std::string& nick) {
    for (Client* c : g_clients) {
        if (c->authenticated && c->nickname == nick) return c;
    }
    return nullptr;
}

static bool nickname_taken(const std::string& nick) {
    bool taken = false;
    pthread_mutex_lock(&g_clients_mutex);
    taken = (find_client_by_nick_locked(nick) != nullptr);
    pthread_mutex_unlock(&g_clients_mutex);
    return taken;
}

static void add_pending_client(int fd, const std::string& addr) {
    Client* c = new Client;
    c->sock = fd;
    c->nickname = "";
    c->authenticated = false;
    c->addr = addr;

    pthread_mutex_lock(&g_clients_mutex);
    g_clients.push_back(c);
    pthread_mutex_unlock(&g_clients_mutex);
}

static void remove_client(int fd, std::string* nick_out = nullptr) {
    pthread_mutex_lock(&g_clients_mutex);
    for (size_t i = 0; i < g_clients.size(); ++i) {
        if (g_clients[i]->sock == fd) {
            if (nick_out) *nick_out = g_clients[i]->nickname;
            delete g_clients[i];
            g_clients.erase(g_clients.begin() + (long)i);
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static bool authenticate_client(int fd, const std::string& nick) {
    pthread_mutex_lock(&g_clients_mutex);

    if (find_client_by_nick_locked(nick) != nullptr) {
        pthread_mutex_unlock(&g_clients_mutex);
        return false;
    }

    Client* c = find_client_by_fd_locked(fd);
    if (c == nullptr) {
        pthread_mutex_unlock(&g_clients_mutex);
        return false;
    }

    c->nickname = nick;
    c->authenticated = true;

    pthread_mutex_unlock(&g_clients_mutex);
    return true;
}

static std::vector<int> authenticated_fds_snapshot() {
    std::vector<int> fds;
    pthread_mutex_lock(&g_clients_mutex);
    for (Client* c : g_clients) {
        if (c->authenticated) fds.push_back(c->sock);
    }
    pthread_mutex_unlock(&g_clients_mutex);
    return fds;
}

static std::vector<std::string> online_users_snapshot() {
    std::vector<std::string> names;
    pthread_mutex_lock(&g_clients_mutex);
    for (Client* c : g_clients) {
        if (c->authenticated) names.push_back(c->nickname);
    }
    pthread_mutex_unlock(&g_clients_mutex);
    return names;
}

static int find_fd_by_nick(const std::string& nick) {
    int fd = -1;
    pthread_mutex_lock(&g_clients_mutex);
    Client* c = find_client_by_nick_locked(nick);
    if (c != nullptr) fd = c->sock;
    pthread_mutex_unlock(&g_clients_mutex);
    return fd;
}

// ------------------------ Высокоуровневая логика ------------------------

static void broadcast_message(const MessageEx& msg, const std::string& app_note) {
    std::vector<int> fds = authenticated_fds_snapshot();
    for (int fd : fds) {
        (void)send_message_ex(fd, msg, app_note);
    }
}

static void send_server_info_to_one(int fd, const std::string& text) {
    MessageEx msg = make_message(
        MSG_SERVER_INFO,
        next_msg_id(),
        "SERVER",
        "",
        std::time(nullptr),
        text
    );
    (void)send_message_ex(fd, msg, "prepare MSG_SERVER_INFO");
}

static void broadcast_server_info(const std::string& text, bool store_in_history) {
    MessageEx msg = make_message(
        MSG_SERVER_INFO,
        next_msg_id(),
        "SERVER",
        "",
        std::time(nullptr),
        text
    );

    if (store_in_history) {
        HistoryRecord rec{};
        rec.msg_id = msg.msg_id;
        rec.timestamp = msg.timestamp;
        rec.sender = "SERVER";
        rec.receiver = "";
        rec.type = "MSG_SERVER_INFO";
        rec.text = text;
        rec.delivered = true;
        rec.is_offline = false;
        append_history_record(rec);
    }

    broadcast_message(msg, "prepare MSG_SERVER_INFO (broadcast)");
}

static void deliver_offline_messages(int fd, const std::string& nickname) {
    std::vector<HistoryRecord> pending;

    pthread_mutex_lock(&g_history_mutex);
    for (const auto& r : g_history) {
        if (r.type == "MSG_PRIVATE" &&
            r.is_offline &&
            !r.delivered &&
            r.receiver == nickname) {
            pending.push_back(r);
        }
    }
    pthread_mutex_unlock(&g_history_mutex);

    if (pending.empty()) {
        log_line("[Application] no offline messages for " + nickname);
        return;
    }

    {
        std::ostringstream oss;
        oss << "[Application] deliver " << pending.size()
            << " offline message(s) to " << nickname;
        log_line(oss.str());
    }

    for (const auto& r : pending) {
        MessageEx msg = make_message(
            MSG_PRIVATE,
            r.msg_id,
            r.sender,
            r.receiver,
            r.timestamp,
            std::string(OFFLINE_MARKER) + r.text
        );

        if (send_message_ex(fd, msg, "prepare MSG_PRIVATE (offline delivery)") == 0) {
            mark_history_delivered(r.msg_id);
        }
    }

    send_server_info_to_one(fd, "Offline messages delivered");
}

static void handle_broadcast_text(const std::string& sender, const std::string& text) {
    uint32_t id = next_msg_id();
    int64_t ts = std::time(nullptr);

    HistoryRecord rec{};
    rec.msg_id = id;
    rec.timestamp = ts;
    rec.sender = sender;
    rec.receiver = "";
    rec.type = "MSG_TEXT";
    rec.text = text;
    rec.delivered = true;
    rec.is_offline = false;
    append_history_record(rec);

    MessageEx msg = make_message(MSG_TEXT, id, sender, "", ts, text);

    {
        std::ostringstream oss;
        oss << "[" << time_to_string(ts) << "][id=" << id << "][" << sender << "]: " << text;
        log_line(oss.str());
    }

    broadcast_message(msg, "prepare MSG_TEXT (broadcast)");
}

static void handle_private_message(int sender_fd,
                                   const std::string& sender,
                                   const std::string& receiver,
                                   const std::string& text) {
    uint32_t id = next_msg_id();
    int64_t ts = std::time(nullptr);

    int target_fd = find_fd_by_nick(receiver);

    HistoryRecord rec{};
    rec.msg_id = id;
    rec.timestamp = ts;
    rec.sender = sender;
    rec.receiver = receiver;
    rec.type = "MSG_PRIVATE";
    rec.text = text;
    rec.is_offline = (target_fd < 0);
    rec.delivered = (target_fd >= 0);
    append_history_record(rec);

    if (target_fd >= 0) {
        MessageEx msg = make_message(MSG_PRIVATE, id, sender, receiver, ts, text);
        (void)send_message_ex(target_fd, msg, "prepare MSG_PRIVATE (direct)");

        std::ostringstream oss;
        oss << "[" << time_to_string(ts) << "][id=" << id << "][PRIVATE]["
            << sender << " -> " << receiver << "]: " << text;
        log_line(oss.str());
    } else {
        log_line("[Application] receiver " + receiver + " is offline");
        log_line("[Application] store message in offline queue");
        send_server_info_to_one(sender_fd, "User [" + receiver + "] is offline. Message stored.");
    }
}

static void handle_list_request(int fd) {
    std::vector<std::string> names = online_users_snapshot();
    std::ostringstream oss;
    oss << "Online users";
    for (const auto& name : names) {
        oss << "\n" << name;
    }
    send_server_info_to_one(fd, oss.str());
}

static void handle_history_request(int fd, const std::string& payload) {
    int count = DEFAULT_HISTORY_COUNT;
    std::string arg = trim_copy(payload);

    if (!arg.empty()) {
        try {
            count = std::stoi(arg);
            if (count <= 0) throw std::runtime_error("bad");
        } catch (...) {
            MessageEx err = make_message(
                MSG_ERROR,
                next_msg_id(),
                "SERVER",
                "",
                std::time(nullptr),
                "Invalid history count"
            );
            (void)send_message_ex(fd, err, "prepare MSG_ERROR (history)");
            return;
        }
    }

    std::vector<HistoryRecord> records = load_history_snapshot_from_file();
    std::ostringstream out;

    if (records.empty()) {
        out << "(no history)";
    } else {
        int start = (int)records.size() - count;
        if (start < 0) start = 0;
        for (size_t i = (size_t)start; i < records.size(); ++i) {
            out << format_history_record(records[i]);
            if (i + 1 < records.size()) out << "\n";
        }
    }

    MessageEx msg = make_message(
        MSG_HISTORY_DATA,
        next_msg_id(),
        "SERVER",
        "",
        std::time(nullptr),
        out.str()
    );
    (void)send_message_ex(fd, msg, "prepare MSG_HISTORY_DATA");
}

// ------------------------ Worker thread ------------------------

static void* worker_main(void*) {
    while (true) {
        int client_fd = pop_job_blocking();
        std::string addr = peer_address(client_fd);

        add_pending_client(client_fd, addr);

        MessageEx msg{};

        // 1) HELLO / WELCOME
        int rc = recv_message_ex(client_fd, msg);
        if (rc <= 0 || msg.type != MSG_HELLO) {
            remove_client(client_fd);
            close(client_fd);
            continue;
        }

        MessageEx welcome = make_message(
            MSG_WELCOME,
            next_msg_id(),
            "SERVER",
            "",
            std::time(nullptr),
            "Welcome " + addr
        );

        if (send_message_ex(client_fd, welcome, "prepare MSG_WELCOME") != 0) {
            remove_client(client_fd);
            close(client_fd);
            continue;
        }

        // 2) AUTH
        std::string nickname;
        bool authenticated = false;

        while (!authenticated) {
            rc = recv_message_ex(client_fd, msg);
            if (rc <= 0) {
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            if (msg.type != MSG_AUTH) {
                log_line("[Application] ignore message until MSG_AUTH");
                continue;
            }

            nickname = trim_copy(msg.payload);
            if (nickname.empty()) nickname = trim_copy(msg.sender);

            if (nickname.empty()) {
                MessageEx err = make_message(
                    MSG_ERROR,
                    next_msg_id(),
                    "SERVER",
                    "",
                    std::time(nullptr),
                    "Empty nickname"
                );
                (void)send_message_ex(client_fd, err, "prepare MSG_ERROR (empty nickname)");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            if ((int)nickname.size() >= MAX_NAME) {
                MessageEx err = make_message(
                    MSG_ERROR,
                    next_msg_id(),
                    "SERVER",
                    "",
                    std::time(nullptr),
                    "Nickname too long"
                );
                (void)send_message_ex(client_fd, err, "prepare MSG_ERROR (nickname too long)");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            if (!authenticate_client(client_fd, nickname)) {
                MessageEx err = make_message(
                    MSG_ERROR,
                    next_msg_id(),
                    "SERVER",
                    "",
                    std::time(nullptr),
                    "Nickname already taken"
                );
                (void)send_message_ex(client_fd, err, "prepare MSG_ERROR (nickname taken)");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            authenticated = true;
            log_line("[Application] authentication success: " + nickname);

            send_server_info_to_one(client_fd, "Authentication successful: " + nickname);

            deliver_offline_messages(client_fd, nickname);

            broadcast_server_info("User [" + nickname + "] connected", true);
        }

        // 3) Main loop
        while (true) {
            rc = recv_message_ex(client_fd, msg);
            if (rc <= 0) {
                remove_client(client_fd);
                close(client_fd);
                log_line("User [" + nickname + "] disconnected");
                broadcast_server_info("User [" + nickname + "] disconnected", true);
                break;
            }

            switch (msg.type) {
                case MSG_TEXT:
                    handle_broadcast_text(nickname, msg.payload);
                    break;

                case MSG_PRIVATE: {
                    std::string receiver = trim_copy(msg.receiver);
                    std::string text = msg.payload;

                    if (receiver.empty()) {
                        MessageEx err = make_message(
                            MSG_ERROR,
                            next_msg_id(),
                            "SERVER",
                            "",
                            std::time(nullptr),
                            "Empty receiver for private message"
                        );
                        (void)send_message_ex(client_fd, err, "prepare MSG_ERROR (private)");
                        break;
                    }

                    handle_private_message(client_fd, nickname, receiver, text);
                    break;
                }

                case MSG_LIST:
                    handle_list_request(client_fd);
                    break;

                case MSG_HISTORY:
                    handle_history_request(client_fd, msg.payload);
                    break;

                case MSG_PING: {
                    MessageEx pong = make_message(
                        MSG_PONG,
                        next_msg_id(),
                        "SERVER",
                        "",
                        std::time(nullptr),
                        "PONG"
                    );
                    (void)send_message_ex(client_fd, pong, "prepare MSG_PONG");
                    break;
                }

                case MSG_BYE: {
                    MessageEx bye = make_message(
                        MSG_BYE,
                        next_msg_id(),
                        "SERVER",
                        "",
                        std::time(nullptr),
                        "BYE"
                    );
                    (void)send_message_ex(client_fd, bye, "prepare MSG_BYE");

                    remove_client(client_fd);
                    close(client_fd);
                    log_line("User [" + nickname + "] disconnected");
                    broadcast_server_info("User [" + nickname + "] disconnected", true);
                    goto next_client;
                }

                default:
                    log_line("[Application] ignore unsupported message: " + msg_type_name(msg.type));
                    break;
            }
        }

    next_client:
        continue;
    }

    return nullptr;
}

// ------------------------ main ------------------------

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int port = std::stoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port" << std::endl;
        return 1;
    }

    // Load history at startup
    pthread_mutex_lock(&g_history_mutex);
    load_history_from_disk_unlocked(g_history);
    uint32_t max_id = 0;
    for (const auto& r : g_history) {
        if (r.msg_id > max_id) max_id = r.msg_id;
    }
    g_next_msg_id = max_id + 1;
    if (g_next_msg_id == 0) g_next_msg_id = 1;
    pthread_mutex_unlock(&g_history_mutex);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    for (int i = 0; i < WORKER_COUNT; ++i) {
        pthread_t th;
        if (pthread_create(&th, nullptr, worker_main, nullptr) != 0) {
            std::cerr << "pthread_create failed" << std::endl;
            close(server_fd);
            return 1;
        }
        pthread_detach(th);
    }

    std::cout << "Server started on port " << port
              << " (workers=" << WORKER_COUNT << ")" << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        std::cout << "Client connected" << std::endl;
        push_job(client_fd);
    }

    close(server_fd);
    return 0;
}