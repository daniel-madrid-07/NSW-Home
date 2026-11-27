// The one place where the friends module touches the system's sockets:
// a UDP socket that only ever sends first and listens for the answers, the
// name lookup for the bootstrap routers, and a steady clock.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace frn {

struct Addr {
    uint32_t ip = 0; // host byte order
    uint16_t port = 0;
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    bool operator<(const Addr& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
};

std::string AddrText(const Addr& a);

// IPv4 addresses of a host name; empty when it cannot be resolved. Blocking.
std::vector<Addr> Resolve(const std::string& host, uint16_t port);

// Seconds on a clock that never goes back.
double Now();

class Udp {
public:
    Udp() = default;
    ~Udp() { Close(); }
    Udp(const Udp&) = delete;
    Udp& operator=(const Udp&) = delete;
    // The socket is never bound by us: the system gives it a port on the first
    // send, and only answers to what was sent come back. (An explicit bind is
    // what makes Windows Firewall ask the user about the program.)
    bool Open();
    void Close();
    bool IsOpen() const { return sock_ != kNone; }
    bool Send(const Addr& to, const std::string& data);
    // 1: a datagram was read; 0: nothing within the time; -1: the socket failed.
    int Receive(Addr& from, std::string& data, int timeoutMs);

private:
    static constexpr intptr_t kNone = -1;
    intptr_t sock_ = kNone;
};

} // namespace frn
