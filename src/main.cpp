#include "controller.hpp"
#include "proc_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

enum class SortKey { Cpu, Mem, Pid };

struct Options {
    bool watch{false};
    int interval_ms{1000};
    std::size_t limit{25};
    SortKey sort{SortKey::Cpu};
    Action action;
};

void print_usage(const char* app) {
    std::cout
        << "Usage:\n"
        << "  " << app << " [--watch] [--interval ms] [--limit n] [--sort cpu|mem|pid]\n"
        << "  " << app << " --kill <pid> [--force]\n"
        << "  " << app << " --stop <pid>\n"
        << "  " << app << " --cont <pid>\n"
        << "  " << app << " --renice <pid> <nice(-20..19)>\n";
}

bool parse_int(const std::string& s, int& value) {
    try {
        std::size_t idx = 0;
        value = std::stoi(s, &idx);
        return idx == s.size();
    } catch (...) {
        return false;
    }
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options opts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--watch") {
            opts.watch = true;
        } else if (arg == "--interval" && i + 1 < argc) {
            int value = 0;
            if (!parse_int(argv[++i], value) || value <= 0) {
                return std::nullopt;
            }
            opts.interval_ms = value;
        } else if (arg == "--limit" && i + 1 < argc) {
            int value = 0;
            if (!parse_int(argv[++i], value) || value <= 0) {
                return std::nullopt;
            }
            opts.limit = static_cast<std::size_t>(value);
        } else if (arg == "--sort" && i + 1 < argc) {
            const std::string key = argv[++i];
            if (key == "cpu") {
                opts.sort = SortKey::Cpu;
            } else if (key == "mem") {
                opts.sort = SortKey::Mem;
            } else if (key == "pid") {
                opts.sort = SortKey::Pid;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--kill" && i + 1 < argc) {
            int pid = 0;
            if (!parse_int(argv[++i], pid)) {
                return std::nullopt;
            }
            opts.action = Action{ActionType::Kill, pid, std::nullopt};
        } else if (arg == "--force") {
            if (opts.action.type != ActionType::Kill) {
                return std::nullopt;
            }
            opts.action.type = ActionType::ForceKill;
        } else if (arg == "--stop" && i + 1 < argc) {
            int pid = 0;
            if (!parse_int(argv[++i], pid)) {
                return std::nullopt;
            }
            opts.action = Action{ActionType::Stop, pid, std::nullopt};
        } else if (arg == "--cont" && i + 1 < argc) {
            int pid = 0;
            if (!parse_int(argv[++i], pid)) {
                return std::nullopt;
            }
            opts.action = Action{ActionType::Continue, pid, std::nullopt};
        } else if (arg == "--renice" && i + 2 < argc) {
            int pid = 0;
            int nice = 0;
            if (!parse_int(argv[++i], pid) || !parse_int(argv[++i], nice)) {
                return std::nullopt;
            }
            opts.action = Action{ActionType::Renice, pid, nice};
        } else {
            return std::nullopt;
        }
    }

    return opts;
}

void print_table(const std::vector<ProcessView>& views, std::size_t limit) {
    constexpr int comm_width = 100;
    std::cout << std::left << std::setw(8) << "PID" << std::setw(comm_width) << "COMM" << std::right
              << std::setw(10) << "CPU %" << std::setw(12) << "RSS(MB)" << '\n';
    std::cout << std::string(8 + comm_width + 10 + 12, '-') << '\n';

    std::size_t count = std::min(limit, views.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = views[i];
        const double rss_mb = static_cast<double>(p.rss_kb) / 1024.0;
        std::cout << std::left << std::setw(8) << p.pid << std::setw(comm_width) << p.comm << std::right
                  << std::setw(10) << std::fixed << std::setprecision(2) << p.cpu_percent << std::setw(12)
                  << std::setprecision(1) << rss_mb << '\n';
    }
}

void sort_views(std::vector<ProcessView>& views, SortKey key) {
    switch (key) {
    case SortKey::Cpu:
        std::sort(views.begin(), views.end(), [](const ProcessView& a, const ProcessView& b) {
            return a.cpu_percent > b.cpu_percent;
        });
        break;
    case SortKey::Mem:
        std::sort(views.begin(), views.end(), [](const ProcessView& a, const ProcessView& b) {
            return a.rss_kb > b.rss_kb;
        });
        break;
    case SortKey::Pid:
        std::sort(views.begin(), views.end(), [](const ProcessView& a, const ProcessView& b) {
            return a.pid < b.pid;
        });
        break;
    }
}

int main(int argc, char** argv) {
    auto opts = parse_options(argc, argv);
    if (!opts) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts->action.type != ActionType::None) {
        std::cout << execute_action(opts->action) << '\n';
        return 0;
    }

    ProcReader reader;
    const long clock_ticks = sysconf(_SC_CLK_TCK);

    auto prev_samples = reader.read_processes();
    std::unordered_map<int, ProcessSample> prev_map;
    prev_map.reserve(prev_samples.size());
    for (const auto& p : prev_samples) {
        prev_map[p.pid] = p;
    }

    auto prev_time = std::chrono::steady_clock::now();

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opts->interval_ms));

        auto curr = reader.read_processes();
        const auto curr_time = std::chrono::steady_clock::now();
        const double elapsed_seconds = std::chrono::duration<double>(curr_time - prev_time).count();

        auto views = build_views(curr, prev_map, elapsed_seconds, clock_ticks);
        sort_views(views, opts->sort);

        if (opts->watch) {
            std::cout << "\033[2J\033[H";
        }
        print_table(views, opts->limit);

        prev_map.clear();
        prev_map.reserve(curr.size());
        for (const auto& p : curr) {
            prev_map[p.pid] = p;
        }
        prev_time = curr_time;

        if (!opts->watch) {
            break;
        }
    }

    return 0;
}
