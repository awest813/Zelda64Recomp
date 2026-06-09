// C++ library shims for KOS GCC 9 / newlib (syntax-check and Dreamcast builds).
#pragma once

#include <string>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fenv.h>

#ifndef FE_TONEAREST
#define FE_TONEAREST 0
#define FE_TOWARDZERO 3
#define FE_UPWARD 2
#define FE_DOWNWARD 1
inline int fegetround() { return FE_TONEAREST; }
inline int fesetround(int) { return 0; }
#endif

// newlib exposes C99 math/stdio in the global namespace only.
namespace std {
using ::snprintf;
using ::strtof;
using ::strtold;
using ::strtoull;
using ::strtoll;
using ::lround;
inline unsigned long long stoull(const std::string& str, std::size_t* idx = nullptr, int base = 10) {
    return ::strtoull(str.c_str(), idx != nullptr ? reinterpret_cast<char**>(idx) : nullptr, base);
}
} // namespace std

#if defined(__GNUC__) && (__GNUC__ < 10)
namespace std {
inline std::string to_string(long long value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", value);
    return buf;
}
inline std::string to_string(unsigned long long value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", value);
    return buf;
}
inline std::string to_string(int value) { return to_string(static_cast<long long>(value)); }
inline std::string to_string(unsigned value) { return to_string(static_cast<unsigned long long>(value)); }
inline std::string to_string(long value) { return to_string(static_cast<long long>(value)); }
inline std::string to_string(unsigned long value) { return to_string(static_cast<unsigned long long>(value)); }
inline std::string to_string(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    return buf;
}
inline std::string to_string(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", value);
    return buf;
}
inline std::string to_string(long double value) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%Lg", value);
    return buf;
}
#if !defined(_GLIBCXX_MUTEX)
// Minimal threading primitives for bring-up when libstdc++ omits <mutex>.
class mutex {
public:
    void lock() {}
    void unlock() {}
};
class thread {
public:
    template <typename F>
    explicit thread(F&&) {}
    void detach() {}
};
#endif
} // namespace std
#endif
