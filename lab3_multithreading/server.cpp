#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

#define MAX_PAYLOAD 1024
static const int WORKERS = 10;

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

// protocol: [uint32 length][uint8 type][payload...], length = 1 + payload_len
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

// ---------- global job queue ----------
static std::deque<int> g_jobs;
static pthread_mutex_t g_jobs_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_jobs_cv  = PTHREAD_COND_INITIALIZER;

// ---------- global client list ----------
struct ClientInfo {
    int fd;
    std::string nick;
    std::string addr; // "ip:port"
};

static std::vector<ClientInfo> g_clients;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

static std::string fd_peer_addr(int fd) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (sockaddr*)&peer, &len) != 0) return "unknown:0";

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    uint16_t port = ntohs(peer.sin_port);
    return std::string(ip) + ":" + std::to_string(port);
}

static void add_client(int fd, const std::string& nick, const std::string& addr) {
    pthread_mutex_lock(&g_clients_mtx);
    g_clients.push_back(ClientInfo{fd, nick, addr});
    pthread_mutex_unlock(&g_clients_mtx);
}

static void remove_client(int fd, std::string* out_nick = nullptr, std::string* out_addr = nullptr) {
    pthread_mutex_lock(&g_clients_mtx);
    for (size_t i = 0; i < g_clients.size(); ++i) {
        if (g_clients[i].fd == fd) {
            if (out_nick) *out_nick = g_clients[i].nick;
            if (out_addr) *out_addr = g_clients[i].addr;
            g_clients.erase(g_clients.begin() + (long)i);
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

static void broadcast_text(const std::string& text) {
    std::vector<int> fds;
    fds.reserve(64);

    pthread_mutex_lock(&g_clients_mtx);
    for (const auto& c : g_clients) fds.push_back(c.fd);
    pthread_mutex_unlock(&g_clients_mtx);

    for (int fd : fds) {
        (void)send_message_str(fd, MSG_TEXT, text);
    }
}

static int pop_job_blocking() {
    pthread_mutex_lock(&g_jobs_mtx);
    while (g_jobs.empty()) {
        pthread_cond_wait(&g_jobs_cv, &g_jobs_mtx);
    }
    int fd = g_jobs.front();
    g_jobs.pop_front();
    pthread_mutex_unlock(&g_jobs_mtx);
    return fd;
}

// ---------- worker thread ----------
static void* worker_main(void*) {
    while (true) {
        int cfd = pop_job_blocking();
        std::string addr = fd_peer_addr(cfd);

        Message msg{};
        std::string payload;

        int rc = recv_message(cfd, msg, payload);
        if (rc <= 0 || msg.type != MSG_HELLO) {
            ::close(cfd);
            continue;
        }

        std::string nick = payload.empty() ? "Client" : payload;

        std::string welcome = "Welcome " + nick + " (" + addr + ")";
        if (send_message_str(cfd, MSG_WELCOME, welcome) != 0) {
            ::close(cfd);
            continue;
        }

        add_client(cfd, nick, addr);

        while (true) {
            rc = recv_message(cfd, msg, payload);
            if (rc <= 0) {
                std::string nn, aa;
                remove_client(cfd, &nn, &aa);
                std::cout << "Client disconnected: " << nn << " [" << aa << "]\n";
                ::close(cfd);
                break;
            }

            if (msg.type == MSG_TEXT) {
                std::string out = nick + " [" + addr + "]: " + payload;
                std::cout << out << "\n";
                broadcast_text(out);
            } else if (msg.type == MSG_PING) {
                (void)send_message(cfd, MSG_PONG, nullptr, 0);
            } else if (msg.type == MSG_BYE) {
                (void)send_message(cfd, MSG_BYE, nullptr, 0);
                std::string nn, aa;
                remove_client(cfd, &nn, &aa);
                std::cout << "Client disconnected: " << nn << " [" << aa << "]\n";
                ::close(cfd);
                break;
            } else {
                // ignore unknown
            }
        }
    }
    return nullptr;
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

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons((uint16_t)port);

    if (::bind(s, (sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind");
        ::close(s);
        return 1;
    }
    if (::listen(s, 128) < 0) {
        perror("listen");
        ::close(s);
        return 1;
    }

    // start pool
    for (int i = 0; i < WORKERS; ++i) {
        pthread_t th;
        if (pthread_create(&th, nullptr, worker_main, nullptr) != 0) {
            std::cerr << "pthread_create failed\n";
            ::close(s);
            return 1;
        }
        pthread_detach(th);
    }

    std::cout << "Server started on port " << port << " (workers=" << WORKERS << ")\n";

    // accept loop -> push to queue
    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int cfd = ::accept(s, (sockaddr*)&cli, &len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&g_jobs_mtx);
        g_jobs.push_back(cfd);
        pthread_mutex_unlock(&g_jobs_mtx);
        pthread_cond_signal(&g_jobs_cv);
    }

    ::close(s);
    return 0;
}