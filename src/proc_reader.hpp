#pragma once

#include "process.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

class ProcReader {
public:
    std::vector<ProcessSample> read_processes() const;
    std::optional<std::uint64_t> read_total_cpu_ticks() const;

private:
    std::optional<ProcessSample> read_process(int pid) const;
    static bool is_numeric(const std::string& value);
};

std::vector<ProcessView> build_views(
    const std::vector<ProcessSample>& current,
    const std::unordered_map<int, ProcessSample>& previous,
    double elapsed_seconds,
    long clock_ticks_per_second);
