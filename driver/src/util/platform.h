// Platform abstraction macros. Everything OS/compiler specific that is not
// socket related lives here (sockets: net/udp_socket.*).
#pragma once

#if defined(_WIN32)
#define VHMD_EXPORT extern "C" __declspec(dllexport)
#define VHMD_PLATFORM_WINDOWS 1
#elif defined(__GNUC__) || defined(__clang__)
#define VHMD_EXPORT extern "C" __attribute__((visibility("default")))
#define VHMD_PLATFORM_POSIX 1
#else
#error "Unsupported platform"
#endif

namespace vhmd {

// Path separator used when composing filesystem paths by hand.
#if defined(VHMD_PLATFORM_WINDOWS)
inline constexpr char kPathSeparator = '\\';
#else
inline constexpr char kPathSeparator = '/';
#endif

} // namespace vhmd
