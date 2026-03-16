// udp_client.cpp (macOS)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

// static void set_recv_timeout(int sock, int timeout_ms) {
//     timeval tv{};
//     tv.tv_sec = timeout_ms / 1000;
//     tv.tv_usec = (timeout_ms % 1000) * 1000;
//     setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
// }

int main(int argc, char** argv) {
    std::string server_ip = argv[1];
    int port = std::stoi(argv[2]);
    std::string msg;

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    // set_recv_timeout(sock, 2000); // 2 секунды

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, server_ip.c_str(), &srv.sin_addr) != 1) {
        std::cerr << "inet_pton() failed (bad IP)\n";
        close(sock);
        return 1;
    }

    while (true) {
        std::cout << "введите сообщение: ";
        std::cin >> msg;

        sendto(sock, msg.c_str(), msg.size(), 0,
                            reinterpret_cast<sockaddr*>(&srv), sizeof(srv));


        char buf[2048];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);

        recvfrom(sock, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);

        std::cout << "Reply: " << buf << "\n";
    }


    close(sock);
    return 0;
}
