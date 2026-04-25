#include "proc_reader.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::optional<std::uint64_t> parse_uint64(const std::string& token) {
    std::uint64_t value = 0;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> parse_int(const std::string& token) {
    int value = 0;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}
} // namespace

bool ProcReader::is_numeric(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), ::isdigit);
}

std::optional<std::uint64_t> ProcReader::read_total_cpu_ticks() const {
    std::ifstream file("/proc/stat");
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }

    std::istringstream iss(line);
    std::string label;
    iss >> label;
    if (label != "cpu") {
        return std::nullopt;
    }

    std::uint64_t total = 0;
    std::uint64_t part = 0;
    while (iss >> part) {
        total += part;
    }
    return total;
}

std::optional<ProcessSample> ProcReader::read_process(int pid) const {
    const std::string base = "/proc/" + std::to_string(pid);

    std::ifstream stat_file(base + "/stat");
    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) {
        return std::nullopt;
    }

    auto close_pos = stat_line.rfind(')');
    auto open_pos = stat_line.find('(');
    if (open_pos == std::string::npos || close_pos == std::string::npos || close_pos <= open_pos) {
        return std::nullopt;
    }

    std::string comm = stat_line.substr(open_pos + 1, close_pos - open_pos - 1);
    std::string remainder = stat_line.substr(close_pos + 2);

    std::istringstream fields(remainder);
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token) {
        tokens.push_back(token);
    }

    // In remainder tokens, utime/stime/nice are indices 11, 12, 16.
    if (tokens.size() < 17) {
        return std::nullopt;
    }

    auto utime = parse_uint64(tokens[11]);
    auto stime = parse_uint64(tokens[12]);
    auto nice = parse_int(tokens[16]);
    if (!utime.has_value() || !stime.has_value() || !nice.has_value()) {
        return std::nullopt;
    }

    std::uint64_t rss_kb = 0;
    std::ifstream status_file(base + "/status");
    while (std::getline(status_file, token)) {
        if (token.rfind("VmRSS:", 0) == 0) {
            std::istringstream mem_stream(token.substr(6));
            mem_stream >> rss_kb;
            break;
        }
    }

    std::ifstream cmdline_file(base + "/cmdline", std::ios::binary);
    std::string argv0;
    if (cmdline_file) {
        std::getline(cmdline_file, argv0, '\0');
    }

    if (comm.empty()) {
        std::ifstream comm_file(base + "/comm");
        std::string comm_line;
        if (std::getline(comm_file, comm_line) && !comm_line.empty()) {
            comm = comm_line;
        }
    }

    if (!argv0.empty()) {
        const auto base_name = std::filesystem::path(argv0).filename().string();
        if (!base_name.empty() && base_name.rfind(comm, 0) == 0 && base_name.size() > comm.size()) {
            comm = base_name;
        }
    }

    return ProcessSample{pid, comm, *utime, *stime, *nice, rss_kb};
}

std::vector<ProcessSample> ProcReader::read_processes() const {
    std::vector<ProcessSample> processes;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        if (!entry.is_directory()) {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (!is_numeric(name)) {
            continue;
        }

        int pid = 0;
        auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (ec != std::errc{} || ptr != name.data() + name.size()) {
            continue;
        }

        if (auto sample = read_process(pid)) {
            processes.push_back(*sample);
        }
    }
    return processes;
}

std::vector<ProcessView> build_views(
    const std::vector<ProcessSample>& current,
    const std::unordered_map<int, ProcessSample>& previous,
    double elapsed_seconds,
    long clock_ticks_per_second) {
    std::vector<ProcessView> result;
    result.reserve(current.size());

    for (const auto& proc : current) {
        double cpu = 0.0;
        const auto it = previous.find(proc.pid);
        if (it != previous.end() && elapsed_seconds > 0.0 && clock_ticks_per_second > 0) {
            const auto prev_total = it->second.utime_ticks + it->second.stime_ticks;
            const auto cur_total = proc.utime_ticks + proc.stime_ticks;
            const auto proc_delta = cur_total > prev_total ? cur_total - prev_total : 0;
            const auto elapsed_ticks = elapsed_seconds * static_cast<double>(clock_ticks_per_second);
            cpu = static_cast<double>(proc_delta) * 100.0 / elapsed_ticks;
        }

        result.push_back(ProcessView{proc.pid, proc.comm, cpu, proc.nice, proc.rss_kb});
    }

    return result;
}
