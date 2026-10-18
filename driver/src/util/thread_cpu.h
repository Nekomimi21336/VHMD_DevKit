// CPU time consumed by the calling thread (for the driver's self-reported load).
#pragma once

namespace vhmd {

// Seconds of user + kernel CPU time used by the calling thread so far.
double CurrentThreadCpuSeconds();

} // namespace vhmd
