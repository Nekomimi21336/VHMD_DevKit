#include "net/udp_socket.h"

#include "util/platform.h"

#if defined(VHMD_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mutex>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using socket_t = SOCKET;
static const intptr_t kInvalidHandle = static_cast<intptr_t>(INVALID_SOCKET);
static int CloseSocket(socket_t s) { return closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const intptr_t kInvalidHandle = -1;
static int CloseSocket(socket_t s) { return close(s); }
#endif

#include <cstdio>
#include <cstring>

namespace vhmd {
namespace {

#if defined(VHMD_PLATFORM_WINDOWS)
// WSAStartup is reference counted by WinSock itself; keep one reference for
// the lifetime of the process once any socket has been created.
bool EnsureSocketLibrary() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}
#else
bool EnsureSocketLibrary() { return true; }
#endif

socket_t S(intptr_t h) { return static_cast<socket_t>(h); }

} // namespace

std::string Endpoint::ToString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ipv4_host_order >> 24) & 0xff, (ipv4_host_order >> 16) & 0xff,
                  (ipv4_host_order >> 8) & 0xff, ipv4_host_order & 0xff, port);
    return buf;
}

UdpSocket::UdpSocket() : handle_(kInvalidHandle) {}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::ResolveIpv4(const std::string& address, uint32_t& out_host_order) {
    if (address == "localhost") {
        out_host_order = 0x7f000001u;
        return true;
    }
    in_addr a{};
    if (inet_pton(AF_INET, address.c_str(), &a) != 1) return false;
    out_host_order = ntohl(a.s_addr);
    return true;
}

bool UdpSocket::Open(const std::string& bind_address, uint16_t port, std::string& error) {
    Close();
    if (!EnsureSocketLibrary()) {
        error = "socket library initialization failed";
        return false;
    }
    uint32_t host = 0;
    if (!ResolveIpv4(bind_address, host)) {
        error = "invalid bind address '" + bind_address + "'";
        return false;
    }
    // Not inheritable: vrserver starts other processes (compositor, home app
    // launchers). An inherited copy outlives a crashed vrserver and keeps the
    // port, so the next SteamVR session could not bind it.
#if defined(VHMD_PLATFORM_WINDOWS)
    socket_t s = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                            WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    socket_t s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
#endif
    if (static_cast<intptr_t>(s) == kInvalidHandle) {
        error = "socket() failed";
        return false;
    }
#if defined(VHMD_PLATFORM_WINDOWS)
    // Do not let another process steal the port; also stops ICMP "port
    // unreachable" from a previous SendTo turning recvfrom into an error.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    BOOL no_reset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(host);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocket(s);
        error = "bind(" + bind_address + ":" + std::to_string(port) + ") failed (port in use?)";
        return false;
    }
    handle_ = static_cast<intptr_t>(s);
    return true;
}

void UdpSocket::Close() {
    if (handle_ != kInvalidHandle) {
        CloseSocket(S(handle_));
        handle_ = kInvalidHandle;
    }
}

bool UdpSocket::IsOpen() const { return handle_ != kInvalidHandle; }

uint16_t UdpSocket::LocalPort() const {
    if (!IsOpen()) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(S(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

bool UdpSocket::IsInheritable() const {
    if (!IsOpen()) return false;
#if defined(VHMD_PLATFORM_WINDOWS)
    DWORD flags = 0;
    return GetHandleInformation(reinterpret_cast<HANDLE>(S(handle_)), &flags) && (flags & HANDLE_FLAG_INHERIT);
#else
    const int flags = fcntl(S(handle_), F_GETFD);
    return flags >= 0 && !(flags & FD_CLOEXEC);
#endif
}

int UdpSocket::Receive(char* buffer, size_t size, Endpoint& from, int timeout_ms) {
    if (!IsOpen()) return -1;
#if defined(VHMD_PLATFORM_WINDOWS)
    WSAPOLLFD pfd{};
    pfd.fd = S(handle_);
    pfd.events = POLLRDNORM;
    int r = WSAPoll(&pfd, 1, timeout_ms);
#else
    pollfd pfd{};
    pfd.fd = S(handle_);
    pfd.events = POLLIN;
    int r = poll(&pfd, 1, timeout_ms);
#endif
    if (r == 0) return 0;
    if (r < 0) return -1;

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int n = static_cast<int>(recvfrom(S(handle_), buffer, static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&addr), &len));
    if (n < 0) {
#if defined(VHMD_PLATFORM_WINDOWS)
        // Oversized datagram: report as "nothing usable" rather than error.
        if (WSAGetLastError() == WSAEMSGSIZE) return 0;
#endif
        return -1;
    }
    from.ipv4_host_order = ntohl(addr.sin_addr.s_addr);
    from.port = ntohs(addr.sin_port);
    return n;
}

bool UdpSocket::SendTo(std::string_view data, const Endpoint& to) {
    if (!IsOpen() || !to.valid()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    addr.sin_addr.s_addr = htonl(to.ipv4_host_order);
    int n = static_cast<int>(sendto(S(handle_), data.data(), static_cast<int>(data.size()), 0,
                                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
    return n == static_cast<int>(data.size());
}

} // namespace vhmd
