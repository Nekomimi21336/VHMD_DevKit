// Thin IPv4 UDP socket wrapper. All WinSock / BSD socket specifics are
// confined to udp_socket.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vhmd {

struct Endpoint {
    uint32_t ipv4_host_order = 0;
    uint16_t port = 0;
    bool valid() const { return port != 0; }
    std::string ToString() const;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // `bind_address` is a dotted IPv4 address ("127.0.0.1", "0.0.0.0") or
    // "localhost". port 0 binds an ephemeral port.
    bool Open(const std::string& bind_address, uint16_t port, std::string& error);
    void Close();
    bool IsOpen() const;
    uint16_t LocalPort() const;
    // True if child processes would inherit the socket (must be false).
    bool IsInheritable() const;

    // Waits up to timeout_ms. Returns bytes received, 0 on timeout, -1 on error.
    int Receive(char* buffer, size_t size, Endpoint& from, int timeout_ms);
    bool SendTo(std::string_view data, const Endpoint& to);

    static bool ResolveIpv4(const std::string& address, uint32_t& out_host_order);

private:
    // Native handle stored as an integer so this header stays OS neutral.
    intptr_t handle_;
};

} // namespace vhmd
