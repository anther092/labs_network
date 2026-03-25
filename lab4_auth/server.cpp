#include <arpa/inet.h>
#include <ctype.h>
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

static const int WORKER_COUNT = 10;
static const int MAX_NICKNAME = 31; // 32 с учетом '\0'

struct Message {
    uint32_t length;              // длина поля type + payload
    uint8_t type;                 // тип сообщения
    char payload[MAX_PAYLOAD];    // данные
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

struct Client {
    int sock;
    char nickname[32];
    int authenticated;
    std::string addr;
};

// ------------------------ Глобальные данные ------------------------

static std::deque<int> g_job_queue;
static pthread_mutex_t g_job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;

static std::vector<Client> g_clients;
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// ------------------------ Вспомогательные функции ------------------------

static const char* message_type_name(uint8_t type) {
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
        default: return "MSG_UNKNOWN";
    }
}

static std::string trim_copy(const std::string& s) {
    size_t left = 0;
    size_t right = s.size();

    while (left < right && std::isspace(static_cast<unsigned char>(s[left]))) {
        ++left;
    }
    while (right > left && std::isspace(static_cast<unsigned char>(s[right - 1]))) {
        --right;
    }

    return s.substr(left, right - left);
}

static std::string get_peer_address(int fd) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);

    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) != 0) {
        return "unknown:0";
    }

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

    return std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
}

// ------------------------ Логи OSI ------------------------

static void osi_in_transport() {
    std::cout << "[Layer 4 - Transport] recv()" << std::endl;
}

static void osi_in_presentation(uint8_t type) {
    std::cout << "[Layer 6 - Presentation] deserialize Message -> "
              << message_type_name(type) << std::endl;
}

static void osi_in_session(const std::string& note) {
    std::cout << "[Layer 5 - Session] " << note << std::endl;
}

static void osi_in_application(const std::string& note) {
    std::cout << "[Layer 7 - Application] " << note << std::endl;
}

static void osi_out_application(const std::string& note) {
    std::cout << "[Layer 7 - Application] " << note << std::endl;
}

static void osi_out_session(const std::string& note) {
    std::cout << "[Layer 5 - Session] " << note << std::endl;
}

static void osi_out_presentation(uint8_t type) {
    std::cout << "[Layer 6 - Presentation] serialize Message <- "
              << message_type_name(type) << std::endl;
}

static void osi_out_transport() {
    std::cout << "[Layer 4 - Transport] send()" << std::endl;
}

// ------------------------ Низкоуровневые send/recv ------------------------

static ssize_t recv_all(int fd, void* buf, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        ssize_t r = recv(fd, static_cast<char*>(buf) + offset, size - offset, 0);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += static_cast<size_t>(r);
    }

    return static_cast<ssize_t>(offset);
}

static ssize_t send_all(int fd, const void* buf, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        ssize_t s = send(fd, static_cast<const char*>(buf) + offset, size - offset, 0);
        if (s < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += static_cast<size_t>(s);
    }

    return static_cast<ssize_t>(offset);
}

// ------------------------ Протокол уровня Presentation ------------------------

static int recv_message_osi(int fd, Message& msg, std::string& payload_out) {
    osi_in_transport();

    uint32_t net_length = 0;
    ssize_t r = recv_all(fd, &net_length, sizeof(net_length));
    if (r == 0) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }

    uint32_t length = ntohl(net_length);
    if (length < 1u || length > 1u + MAX_PAYLOAD) {
        return -2;
    }

    uint8_t type = 0;
    r = recv_all(fd, &type, sizeof(type));
    if (r == 0) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }

    uint32_t payload_len = length - 1u;
    if (payload_len > 0) {
        r = recv_all(fd, msg.payload, payload_len);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            return -1;
        }
    }

    msg.length = length;
    msg.type = type;

    if (payload_len >= MAX_PAYLOAD) {
        payload_len = MAX_PAYLOAD - 1;
    }
    msg.payload[payload_len] = '\0';

    payload_out.assign(msg.payload, msg.payload + payload_len);

    osi_in_presentation(type);
    return 1;
}

static int send_message_osi(int fd,
                            uint8_t type,
                            const std::string& payload,
                            const std::string& app_note,
                            const std::string& session_note) {
    osi_out_application(app_note);
    osi_out_session(session_note);
    osi_out_presentation(type);

    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    if (payload_len > MAX_PAYLOAD) {
        return -1;
    }

    uint32_t length = 1u + payload_len;
    uint32_t net_length = htonl(length);

    std::string buffer;
    buffer.resize(sizeof(net_length) + sizeof(type) + payload_len);

    std::memcpy(&buffer[0], &net_length, sizeof(net_length));
    std::memcpy(&buffer[sizeof(net_length)], &type, sizeof(type));

    if (payload_len > 0) {
        std::memcpy(&buffer[sizeof(net_length) + sizeof(type)],
                    payload.data(),
                    payload_len);
    }

    osi_out_transport();

    if (send_all(fd, buffer.data(), buffer.size()) != static_cast<ssize_t>(buffer.size())) {
        return -1;
    }

    return 0;
}

static int send_message_osi_empty(int fd,
                                  uint8_t type,
                                  const std::string& app_note,
                                  const std::string& session_note) {
    return send_message_osi(fd, type, "", app_note, session_note);
}

// ------------------------ Очередь заданий ------------------------

static void push_job(int client_fd) {
    pthread_mutex_lock(&g_job_mutex);
    g_job_queue.push_back(client_fd);
    pthread_mutex_unlock(&g_job_mutex);
    pthread_cond_signal(&g_job_cond);
}

static int pop_job_blocking() {
    pthread_mutex_lock(&g_job_mutex);

    while (g_job_queue.empty()) {
        pthread_cond_wait(&g_job_cond, &g_job_mutex);
    }

    int client_fd = g_job_queue.front();
    g_job_queue.pop_front();

    pthread_mutex_unlock(&g_job_mutex);
    return client_fd;
}

// ------------------------ Работа со списком клиентов ------------------------

static void add_pending_client(int fd, const std::string& addr) {
    Client client{};
    client.sock = fd;
    client.nickname[0] = '\0';
    client.authenticated = 0;
    client.addr = addr;

    pthread_mutex_lock(&g_clients_mutex);
    g_clients.push_back(client);
    pthread_mutex_unlock(&g_clients_mutex);
}

static void remove_client(int fd, std::string* nickname_out = nullptr) {
    pthread_mutex_lock(&g_clients_mutex);

    for (size_t i = 0; i < g_clients.size(); ++i) {
        if (g_clients[i].sock == fd) {
            if (nickname_out != nullptr) {
                *nickname_out = g_clients[i].nickname;
            }
            g_clients.erase(g_clients.begin() + static_cast<long>(i));
            break;
        }
    }

    pthread_mutex_unlock(&g_clients_mutex);
}

static bool nickname_taken_locked(const std::string& nickname) {
    for (const auto& client : g_clients) {
        if (client.authenticated && nickname == client.nickname) {
            return true;
        }
    }
    return false;
}

static bool authenticate_client(int fd, const std::string& nickname) {
    pthread_mutex_lock(&g_clients_mutex);

    if (nickname_taken_locked(nickname)) {
        pthread_mutex_unlock(&g_clients_mutex);
        return false;
    }

    for (auto& client : g_clients) {
        if (client.sock == fd) {
            std::snprintf(client.nickname, sizeof(client.nickname), "%s", nickname.c_str());
            client.authenticated = 1;
            pthread_mutex_unlock(&g_clients_mutex);
            return true;
        }
    }

    pthread_mutex_unlock(&g_clients_mutex);
    return false;
}

static int find_client_fd_by_nickname(const std::string& nickname) {
    pthread_mutex_lock(&g_clients_mutex);

    for (const auto& client : g_clients) {
        if (client.authenticated && nickname == client.nickname) {
            int fd = client.sock;
            pthread_mutex_unlock(&g_clients_mutex);
            return fd;
        }
    }

    pthread_mutex_unlock(&g_clients_mutex);
    return -1;
}

static std::vector<int> get_authenticated_fds_snapshot() {
    std::vector<int> result;

    pthread_mutex_lock(&g_clients_mutex);
    for (const auto& client : g_clients) {
        if (client.authenticated) {
            result.push_back(client.sock);
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);

    return result;
}

// ------------------------ Логика Application ------------------------

static void broadcast_server_info(const std::string& info) {
    std::vector<int> fds = get_authenticated_fds_snapshot();

    for (int fd : fds) {
        (void)send_message_osi(fd,
                               MSG_SERVER_INFO,
                               info,
                               "prepare MSG_SERVER_INFO (broadcast)",
                               "authenticated session active");
    }
}

static void broadcast_text(const std::string& text) {
    std::vector<int> fds = get_authenticated_fds_snapshot();

    for (int fd : fds) {
        (void)send_message_osi(fd,
                               MSG_TEXT,
                               text,
                               "prepare MSG_TEXT (broadcast)",
                               "authenticated session active");
    }
}

static void send_auth_error_and_close(int fd, const std::string& error_text) {
    (void)send_message_osi(fd,
                           MSG_ERROR,
                           error_text,
                           "prepare MSG_ERROR",
                           "authentication failed, session closing");
}

// ------------------------ Worker ------------------------

static void* worker_main(void*) {
    while (true) {
        int client_fd = pop_job_blocking();
        std::string client_addr = get_peer_address(client_fd);

        add_pending_client(client_fd, client_addr);

        Message msg{};
        std::string payload;

        // -------------------- Этап 1: HELLO / WELCOME --------------------
        int rc = recv_message_osi(client_fd, msg, payload);
        if (rc <= 0) {
            osi_in_session("connection closed during pre-sync");
            osi_in_application("disconnect client");
            remove_client(client_fd);
            close(client_fd);
            continue;
        }

        osi_in_session("pre-sync phase");
        if (msg.type != MSG_HELLO) {
            osi_in_application("expected MSG_HELLO, disconnect client");
            remove_client(client_fd);
            close(client_fd);
            continue;
        }

        osi_in_application("handle MSG_HELLO");

        std::string welcome = "Welcome " + client_addr;
        if (send_message_osi(client_fd,
                             MSG_WELCOME,
                             welcome,
                             "prepare MSG_WELCOME (pre-sync)",
                             "pre-sync completed, session initialized") != 0) {
            remove_client(client_fd);
            close(client_fd);
            continue;
        }

        // -------------------- Этап 2: обязательная аутентификация --------------------
        bool authenticated = false;
        std::string nickname;

        while (!authenticated) {
            rc = recv_message_osi(client_fd, msg, payload);
            if (rc <= 0) {
                osi_in_session("client disconnected before authentication");
                osi_in_application("close unauthenticated session");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            osi_in_session("authentication required");

            if (msg.type != MSG_AUTH) {
                osi_in_application("ignore message until MSG_AUTH");
                continue;
            }

            osi_in_application("handle MSG_AUTH");

            std::string proposed_nick = trim_copy(payload);

            if (proposed_nick.empty()) {
                osi_in_session("authentication failed: empty nickname");
                send_auth_error_and_close(client_fd, "Empty nickname");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            if (static_cast<int>(proposed_nick.size()) > MAX_NICKNAME) {
                osi_in_session("authentication failed: nickname too long");
                send_auth_error_and_close(client_fd, "Nickname too long (max 31)");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            if (!authenticate_client(client_fd, proposed_nick)) {
                osi_in_session("authentication failed: nickname already taken");
                send_auth_error_and_close(client_fd, "Nickname already taken");
                remove_client(client_fd);
                close(client_fd);
                goto next_client;
            }

            nickname = proposed_nick;
            authenticated = true;

            osi_in_session("authentication success");
            std::cout << "User [" << nickname << "] connected" << std::endl;

            broadcast_server_info("User [" + nickname + "] connected");
        }

        // -------------------- Этап 3: основной цикл работы --------------------
        while (true) {
            rc = recv_message_osi(client_fd, msg, payload);
            if (rc <= 0) {
                osi_in_session("authenticated client disconnected");
                osi_in_application("remove client from active session list");

                remove_client(client_fd);
                close(client_fd);

                std::cout << "User [" << nickname << "] disconnected" << std::endl;
                broadcast_server_info("User [" + nickname + "] disconnected");
                break;
            }

            osi_in_session("client authenticated");

            if (msg.type == MSG_TEXT) {
                osi_in_application("handle MSG_TEXT");

                std::string out = "[" + nickname + "]: " + payload;
                std::cout << out << std::endl;

                broadcast_text(out);
            }
            else if (msg.type == MSG_PRIVATE) {
                osi_in_application("handle MSG_PRIVATE");

                // формат payload: target_nick:message
                size_t pos = payload.find(':');
                if (pos == std::string::npos) {
                    (void)send_message_osi(client_fd,
                                           MSG_ERROR,
                                           "Bad private format. Use target:message",
                                           "prepare MSG_ERROR (bad private format)",
                                           "authenticated session active");
                    continue;
                }

                std::string target_nick = trim_copy(payload.substr(0, pos));
                std::string private_text = payload.substr(pos + 1);

                if (target_nick.empty()) {
                    (void)send_message_osi(client_fd,
                                           MSG_ERROR,
                                           "Empty target nickname",
                                           "prepare MSG_ERROR (empty target)",
                                           "authenticated session active");
                    continue;
                }

                int target_fd = find_client_fd_by_nickname(target_nick);
                if (target_fd < 0) {
                    (void)send_message_osi(client_fd,
                                           MSG_ERROR,
                                           "User not found: " + target_nick,
                                           "prepare MSG_ERROR (user not found)",
                                           "authenticated session active");
                    continue;
                }

                std::string formatted_private = "[PRIVATE][" + nickname + "]: " + private_text;

                (void)send_message_osi(target_fd,
                                       MSG_PRIVATE,
                                       formatted_private,
                                       "prepare MSG_PRIVATE (deliver)",
                                       "authenticated session active");
            }
            else if (msg.type == MSG_PING) {
                osi_in_application("handle MSG_PING");

                (void)send_message_osi_empty(client_fd,
                                             MSG_PONG,
                                             "prepare MSG_PONG",
                                             "authenticated session active");
            }
            else if (msg.type == MSG_BYE) {
                osi_in_application("handle MSG_BYE");

                (void)send_message_osi_empty(client_fd,
                                             MSG_BYE,
                                             "prepare MSG_BYE",
                                             "terminate client session");

                remove_client(client_fd);
                close(client_fd);

                std::cout << "User [" << nickname << "] disconnected" << std::endl;
                broadcast_server_info("User [" + nickname + "] disconnected");
                break;
            }
            else if (msg.type == MSG_AUTH) {
                osi_in_application("ignore repeated MSG_AUTH");
            }
            else if (msg.type == MSG_HELLO || msg.type == MSG_WELCOME) {
                osi_in_application("ignore service message in authenticated session");
            }
            else {
                osi_in_application(std::string("ignore unsupported message: ") +
                                   message_type_name(msg.type));
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
    server_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    // Создаем пул потоков
    for (int i = 0; i < WORKER_COUNT; ++i) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, worker_main, nullptr) != 0) {
            std::cerr << "pthread_create failed" << std::endl;
            close(server_fd);
            return 1;
        }
        pthread_detach(thread);
    }

    std::cout << "Server started on port " << port
              << " (workers=" << WORKER_COUNT << ")" << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        std::cout << "Client connected" << std::endl;
        push_job(client_fd);
    }

    close(server_fd);
    return 0;
}
