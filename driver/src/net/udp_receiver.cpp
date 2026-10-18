#include "net/udp_receiver.h"

#include "util/thread_cpu.h"

#include <vector>

namespace vhmd {

UdpReceiver::~UdpReceiver() { Stop(); }

bool UdpReceiver::Start(const std::string& bind_address, uint16_t port, Handler handler, std::string& error) {
    Stop();
    if (!socket_.Open(bind_address, port, error)) return false;
    handler_ = std::move(handler);
    running_ = true;
    thread_ = std::thread(&UdpReceiver::Loop, this);
    return true;
}

void UdpReceiver::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    socket_.Close();
}

void UdpReceiver::Loop() {
    std::vector<char> buffer(65536);
    while (running_) {
        Endpoint from;
        const int n = socket_.Receive(buffer.data(), buffer.size(), from, 100);
        cpu_seconds_ = CurrentThreadCpuSeconds();
        if (n <= 0) continue; // timeout or transient error
        try {
            handler_(std::string_view(buffer.data(), static_cast<size_t>(n)), from);
        } catch (...) {
            // A handler bug must not kill the receive thread (and with it the driver).
        }
    }
}

} // namespace vhmd
