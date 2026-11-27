// See fr_net.h. Winsock on Windows, BSD sockets everywhere else; the calls
// used are the ones the two have in common.
#include "fr_net.h"
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socklen_type = socklen_t;
#endif

namespace frn {
namespace {

bool Startup() {
#ifdef _WIN32
    static bool ok = [] {
        WSADATA w;
        return WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

} // namespace

std::string AddrText(const Addr& a) {
    char b[32];
    snprintf(b, sizeof b, "%u.%u.%u.%u:%u", a.ip >> 24, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, a.port);
    return b;
}

std::vector<Addr> Resolve(const std::string& host, uint16_t port) {
    std::vector<Addr> out;
    if (!Startup()) return out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr) continue;
        sockaddr_in sa;
        memcpy(&sa, p->ai_addr, sizeof sa);
        Addr a;
        a.ip = ntohl(sa.sin_addr.s_addr);
        a.port = port;
        bool seen = false;
        for (const auto& o : out) seen = seen || o == a;
        if (!seen) out.push_back(a);
    }
    freeaddrinfo(res);
    return out;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Udp::Open() {
    Close();
    if (!Startup()) return false;
    auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return false;
    // Windows reports an ICMP "port unreachable" for an earlier send as an
    // error on the next receive; one dead node must not look like a dead socket.
    DWORD off = 0, got = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
#else
    if (s < 0) return false;
#endif
    sock_ = static_cast<intptr_t>(s);
    return true;
}

void Udp::Close() {
    if (sock_ == kNone) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(sock_));
#else
    close(static_cast<int>(sock_));
#endif
    sock_ = kNone;
}

bool Udp::Send(const Addr& to, const std::string& data) {
    if (sock_ == kNone) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to.port);
    sa.sin_addr.s_addr = htonl(to.ip);
#ifdef _WIN32
    int n = sendto(static_cast<SOCKET>(sock_), data.data(), static_cast<int>(data.size()), 0,
                   reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
#else
    auto n = sendto(static_cast<int>(sock_), data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                    sizeof sa);
#endif
    return n == static_cast<decltype(n)>(data.size());
}

int Udp::Receive(Addr& from, std::string& data, int timeoutMs) {
    if (sock_ == kNone) return -1;
    fd_set set;
    FD_ZERO(&set);
#ifdef _WIN32
    FD_SET(static_cast<SOCKET>(sock_), &set);
#else
    if (sock_ >= FD_SETSIZE) return -1;
    FD_SET(static_cast<int>(sock_), &set);
#endif
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(static_cast<int>(sock_) + 1, &set, nullptr, nullptr, &tv);
    if (r == 0) return 0;
    if (r < 0) return -1;
    char buf[2048];
    sockaddr_in sa{};
    socklen_type len = sizeof sa;
#ifdef _WIN32
    int n = recvfrom(static_cast<SOCKET>(sock_), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    // A datagram longer than the buffer, or the reset described in Open: skip it.
    if (n < 0) return 0;
#else
    auto n = recvfrom(static_cast<int>(sock_), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) return 0;
#endif
    from.ip = ntohl(sa.sin_addr.s_addr);
    from.port = ntohs(sa.sin_port);
    data.assign(buf, static_cast<size_t>(n));
    return 1;
}

} // namespace frn
