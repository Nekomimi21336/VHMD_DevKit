#include "test_framework.h"

#include "util/log.h"

int main() {
    // Keep expected "rejected message" warnings out of the test output.
    vhmd::SetLogSink([](vhmd::LogLevel, const std::string&) {});
    int failed_tests = 0;
    for (const auto& t : vt::Registry()) {
        const int before = vt::Failures();
        t.fn();
        const bool ok = vt::Failures() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
        if (!ok) ++failed_tests;
    }
    std::printf("%zu tests, %d failed\n", vt::Registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
