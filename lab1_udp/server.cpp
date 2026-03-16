#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    int port = 9000;
    if (argc >= 2) port = std::stoi(argv[1]);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << std::strerror(errno) << "\n";
        close(sock);
        return 1;
    }

    std::cout << "UDP server listening on port " << port << "...\n";

    char buf[2048];

    while (true) {
        sockaddr_in client{};
        socklen_t client_len = sizeof(client);

        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<sockaddr*>(&client), &client_len);
        if (n < 0) {
            std::cerr << "recvfrom() failed: " << std::strerror(errno) << "\n";
            break;
        }

        buf[n] = '\0';

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        int cport = ntohs(client.sin_port);

        std::cout << "From " << ip << ":" << cport << " => " << buf << "\n";

        std::string reply = "echo: " + std::string(buf);
        ssize_t sent = ::sendto(sock, reply.c_str(), reply.size(), 0,
                                reinterpret_cast<sockaddr*>(&client), client_len);
        if (sent < 0) {
            std::cerr << "sendto() failed: " << std::strerror(errno) << "\n";
        }
    }

    ::close(sock);
    return 0;
}
