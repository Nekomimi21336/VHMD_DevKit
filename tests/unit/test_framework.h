// Tiny dependency-free test harness.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace vt {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& Failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

} // namespace vt

#define VT_CAT2(a, b) a##b
#define VT_CAT(a, b) VT_CAT2(a, b)
#define TEST(name)                                                        \
    static void name();                                                   \
    static vt::Registrar VT_CAT(reg_, name)(#name, name);                 \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++vt::Failures();                                                          \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                          \
    do {                                                                                               \
        const double va_ = (a), vb_ = (b);                                                             \
        if (!(std::abs(va_ - vb_) <= (eps))) {                                                         \
            std::printf("  FAIL %s:%d: %s=%g vs %s=%g (eps %g)\n", __FILE__, __LINE__, #a, va_, #b, vb_, \
                        static_cast<double>(eps));                                                     \
            ++vt::Failures();                                                                          \
        }                                                                                              \
    } while (0)
