// C++ library shims for KOS GCC 9 / newlib (syntax-check and Dreamcast builds).
#pragma once

#include <string>

#include <cstdio>
#include <cstdlib>
#include <cmath>

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
// Minimal threading primitives for syntax-check / bring-up when libstdc++ omits them.
class mutex {
public:
    void lock() {}
    void unlock() {}
};
template <typename Mutex>
class lock_guard {
public:
    explicit lock_guard(Mutex& m) : m_(m) { m_.lock(); }
    ~lock_guard() { m_.unlock(); }
private:
    Mutex& m_;
};
class thread {
public:
    template <typename F>
    explicit thread(F&&) {}
    void detach() {}
};
} // namespace std
#endif
