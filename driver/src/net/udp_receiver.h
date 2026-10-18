// Background UDP receive loop. Owns the socket and its thread; Stop() is
// bounded by the poll timeout (100 ms) so SteamVR shutdown never hangs here.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "net/udp_socket.h"

namespace vhmd {

class UdpReceiver {
public:
    using Handler = std::function<void(std::string_view datagram, const Endpoint& from)>;

    ~UdpReceiver();

    bool Start(const std::string& bind_address, uint16_t port, Handler handler, std::string& error);
    void Stop();
    bool Running() const { return running_; }
    uint16_t Port() const { return socket_.LocalPort(); }

    // Safe to call from any thread while running.
    bool SendTo(std::string_view data, const Endpoint& to) { return socket_.SendTo(data, to); }
    // CPU seconds used by the receive thread so far (sampled after each packet / timeout).
    double CpuSeconds() const { return cpu_seconds_; }

private:
    void Loop();

    UdpSocket socket_;
    Handler handler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<double> cpu_seconds_{0.0};
};

} // namespace vhmd
