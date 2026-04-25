#pragma once

#include <cstdint>
#include <string>

struct ProcessSample {
    int pid{};
    std::string comm;
    std::uint64_t utime_ticks{};
    std::uint64_t stime_ticks{};
    int nice{};
    std::uint64_t rss_kb{};
};

struct ProcessView {
    int pid{};
    std::string comm;
    double cpu_percent{};
    int nice{};
    std::uint64_t rss_kb{};
};
