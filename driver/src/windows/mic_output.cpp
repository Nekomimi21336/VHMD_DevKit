#include "mic_output.h"

#define NOMINMAX
#include <windows.h>

#include <initguid.h>

#include <propkeydef.h>

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/log.h"

using Microsoft::WRL::ComPtr;

namespace vhmd {
namespace {

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring NameOf(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    std::wstring name;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return name;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

// The active output whose name contains `name`, or the default output.
ComPtr<IMMDevice> FindOutput(const std::string& name) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return nullptr;
    if (name == MicPlayer::kDefaultOutput) {
        ComPtr<IMMDevice> dev;
        en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        return dev;
    }
    ComPtr<IMMDeviceCollection> list;
    UINT count = 0;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list)) || FAILED(list->GetCount(&count)))
        return nullptr;
    const std::wstring wanted = Wide(name);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(list->Item(i, &dev)) && NameOf(dev.Get()).find(wanted) != std::wstring::npos) return dev;
    }
    return nullptr;
}

} // namespace

void MicPlayer::SetTarget(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (name == target_) return;
        target_ = name;
    }
    if (!name.empty() && !running_.exchange(true)) thread_ = std::thread(&MicPlayer::Loop, this);
}

void MicPlayer::SetAvoid(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    avoid_ = name;
}

void MicPlayer::Push(const MicMessage& m) {
    if (!Target().empty()) buffer_.Push(m.pcm.data(), m.pcm.size(), m.seq);
}

std::string MicPlayer::Target() const {
    std::lock_guard<std::mutex> lock(mu_);
    return target_;
}

std::string MicPlayer::State() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

void MicPlayer::SetState(const char* state) {
    std::lock_guard<std::mutex> lock(mu_);
    state_ = state;
}

void MicPlayer::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void MicPlayer::Loop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = sizeof(int16_t);
    format.nAvgBytesPerSec = kRate * sizeof(int16_t);
    while (running_) {
        const std::string name = Target();
        if (name.empty()) {
            SetState("off");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        ComPtr<IMMDevice> dev = FindOutput(name);
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        UINT32 size = 0;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (!dev) {
            SetState("no device");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        std::string avoid;
        {
            std::lock_guard<std::mutex> lock(mu_);
            avoid = avoid_;
        }
        if (!avoid.empty() && NameOf(dev.Get()).find(Wide(avoid)) != std::wstring::npos) {
            SetState("same device");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000 /* 40 ms */, 0, &format, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&size);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            Log(LogLevel::Warning, "mic: cannot open \"%s\": 0x%08lx", name.c_str(), static_cast<unsigned long>(hr));
            SetState(hr == AUDCLNT_E_DEVICE_IN_USE ? "device busy" : "device error");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        SetState("playing");
        Log(LogLevel::Info, "mic: playing into \"%s\"", name.c_str());
        const UINT32 period = kRate / 100;   // 10 ms at a time: silence must not queue up ahead of the sound
        while (running_ && Target() == name) {
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 frames = std::min(size - padding, period);
            if (frames == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(frames, &data))) break;
            buffer_.Pull(reinterpret_cast<int16_t*>(data), frames);
            render->ReleaseBuffer(frames, 0);
        }
        client->Stop();
    }
    SetState("off");
    CloseHandle(ready);
    if (SUCCEEDED(co)) CoUninitialize();
}

void MicOutput::SetDevice(const std::string& name) {
    output_.SetTarget(name);
    monitor_.SetAvoid(name);
}

void MicOutput::Push(const MicMessage& m) {
    output_.Push(m);
    monitor_.Push(m);
}

std::string MicOutput::StatusJson() const {
    const MicBuffer::Stats s = output_.Stats();
    return nlohmann::json{{"device", output_.Target()}, {"state", output_.State()}, {"monitor", monitor_.State()},
                          {"packets", s.packets},       {"lost", s.lost},          {"underruns", s.underruns},
                          {"skipped", s.skipped}}
        .dump();
}

void MicOutput::Stop() {
    output_.Stop();
    monitor_.Stop();
}

} // namespace vhmd
