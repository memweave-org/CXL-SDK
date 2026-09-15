#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace units {

constexpr uint64_t operator"" _B(unsigned long long bytes) {
    return bytes;
}
constexpr uint64_t operator"" _KB(unsigned long long kilobytes) {
    return kilobytes * 1024ULL;
}
constexpr uint64_t operator"" _MB(unsigned long long megabytes) {
    return megabytes * 1024ULL * 1024ULL;
}
constexpr uint64_t operator"" _GB(unsigned long long gigabytes) {
    return gigabytes * 1024ULL * 1024ULL * 1024ULL;
}
constexpr uint64_t operator"" _TB(unsigned long long terabytes) {
    return terabytes * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
}

inline std::string byte_format(uint64_t bytes, int precision = 2) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    size_t unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < sizeof(units) / sizeof(units[0]) - 1) {
        size /= 1024.0;
        ++unit_index;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << size << ' ' << units[unit_index];
    return oss.str();
}

inline uint64_t byte_parse(const std::string& input) {
    double number = 0.0;
    std::string unit;
    std::istringstream iss(input);
    iss >> number >> unit;

    if (unit == "B")
        return static_cast<uint64_t>(number);
    if (unit == "KB")
        return static_cast<uint64_t>(number * 1024ULL);
    if (unit == "MB")
        return static_cast<uint64_t>(number * 1024ULL * 1024ULL);
    if (unit == "GB")
        return static_cast<uint64_t>(number * 1024ULL * 1024ULL * 1024ULL);
    if (unit == "TB")
        return static_cast<uint64_t>(number * 1024ULL * 1024ULL * 1024ULL * 1024ULL);

    throw std::invalid_argument("Unknown unit in input: " + unit);
}

}  // namespace units
