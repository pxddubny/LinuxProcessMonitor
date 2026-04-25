#include "controller.hpp"
#include "proc_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <sys/resource.h>
#include <signal.h>
#include <termios.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

enum class SortKey { Cpu, Mem, Pid };
volatile sig_atomic_t g_should_exit = 0;

struct Options {
    bool watch{false};
    int interval_ms{1000};
    std::size_t limit{25};
    SortKey sort{SortKey::Cpu};
    Action action;
};

class TerminalRawMode {
public:
    TerminalRawMode() {
        const bool stdin_tty = isatty(STDIN_FILENO);
        const bool stdout_tty = isatty(STDOUT_FILENO);
        if (!stdin_tty) {
            return;
        }
        if (tcgetattr(STDIN_FILENO, &old_termios_) != 0) {
            return;
        }

        struct termios raw = old_termios_;
        raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            return;
        }
        enabled_ = true;
        ansi_enabled_ = stdout_tty;
        if (ansi_enabled_) {
            std::cout << "\033[?1049h\033[2J\033[H\033[?25l"; // alternate screen + hide cursor
        }
    }

    ~TerminalRawMode() {
        if (enabled_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old_termios_);
        }
        if (ansi_enabled_) {
            std::cout << "\033[0m\033[?25h\033[?1049l"; // reset + show cursor + leave alternate screen
        }
    }

    TerminalRawMode(const TerminalRawMode&) = delete;
    TerminalRawMode& operator=(const TerminalRawMode&) = delete;

private:
    bool enabled_{false};
    bool ansi_enabled_{false};
    struct termios old_termios_ {};
};

void signal_handler(int) {
    g_should_exit = 1;
}

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
}

void print_usage(const char* app) {
    std::cout
        << "Usage:\n"
        << "  " << app << " [--watch] [--interval ms] [--limit n] [--sort cpu|mem|pid]\n"
        << "\nInteractive TUI keys (with --watch):\n"
        << "  q: quit | j/k: move | s: change sort\n"
        << "  t: SIGTERM | x: SIGKILL | p: SIGSTOP | c: SIGCONT\n"
        << "  +/-: renice selected process\n"
        << "\nOne-shot control commands:\n"
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

const char* sort_name(SortKey key) {
    switch (key) {
    case SortKey::Cpu:
        return "cpu";
    case SortKey::Mem:
        return "mem";
    case SortKey::Pid:
        return "pid";
    }
    return "cpu";
}

SortKey next_sort(SortKey key) {
    switch (key) {
    case SortKey::Cpu:
        return SortKey::Mem;
    case SortKey::Mem:
        return SortKey::Pid;
    case SortKey::Pid:
        return SortKey::Cpu;
    }
    return SortKey::Cpu;
}

void print_table(const std::vector<ProcessView>& views, std::size_t limit) {
    constexpr int comm_width = 100;
    std::cout << std::left << std::setw(8) << "PID" << std::setw(comm_width) << "COMM" << std::right
              << std::setw(6) << "NI" << std::setw(10) << "CPU %" << std::setw(12) << "RSS(MB)" << '\n';
    std::cout << std::string(8 + comm_width + 6 + 10 + 12, '-') << '\n';

    std::size_t count = std::min(limit, views.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = views[i];
        const double rss_mb = static_cast<double>(p.rss_kb) / 1024.0;
        std::cout << std::left << std::setw(8) << p.pid << std::setw(comm_width) << p.comm << std::right
                  << std::setw(6) << p.nice << std::setw(10) << std::fixed << std::setprecision(2)
                  << p.cpu_percent << std::setw(12) << std::setprecision(1) << rss_mb << '\n';
    }
}

void draw_tui(const std::vector<ProcessView>& views,
              std::size_t selected,
              std::size_t top_index,
              std::size_t limit,
              int interval_ms,
              SortKey sort,
              const std::string& status) {
    constexpr int comm_width = 100;

    const bool color = isatty(STDOUT_FILENO);
    if (color) {
        std::cout << "\033[H\033[J";
        std::cout << "\033[1;36m";
    }
    std::cout << "Linux Process Monitor TUI | q quit | j/k move | s sort | t/x/p/c signals | +/- renice\n";
    if (color) {
        std::cout << "\033[0m";
    }
    std::cout << "Sort: " << sort_name(sort) << " | Interval: " << interval_ms
              << " ms | Processes: " << views.size() << '\n';
    if (color) {
        std::cout << "\033[1;32m";
    }
    std::cout << "Status: " << status;
    if (color) {
        std::cout << "\033[0m";
    }
    std::cout << '\n';

    if (color) {
        std::cout << "\033[1;34m";
    }
    std::cout << std::left << std::setw(2) << " " << std::setw(8) << "PID" << std::setw(comm_width) << "COMM"
              << std::right << std::setw(6) << "NI" << std::setw(10) << "CPU %" << std::setw(12) << "RSS(MB)"
              << '\n';
    if (color) {
        std::cout << "\033[0m";
    }
    std::cout << std::string(2 + 8 + comm_width + 6 + 10 + 12, '-') << '\n';

    const std::size_t end = std::min(views.size(), top_index + limit);
    for (std::size_t i = top_index; i < end; ++i) {
        const auto& p = views[i];
        const bool is_selected = (i == selected);
        const double rss_mb = static_cast<double>(p.rss_kb) / 1024.0;

        if (is_selected && color) {
            std::cout << "\033[7m";
        }
        std::cout << (is_selected ? ">" : " ") << " ";
        std::cout << std::left << std::setw(8) << p.pid << std::setw(comm_width) << p.comm;

        if (color && p.cpu_percent >= 50.0) {
            std::cout << "\033[31m";
        } else if (color && p.cpu_percent >= 20.0) {
            std::cout << "\033[33m";
        }

        std::cout << std::right << std::setw(6) << p.nice;
        std::cout << std::setw(10) << std::fixed << std::setprecision(2) << p.cpu_percent;
        if (color) {
            std::cout << "\033[0m";
        }
        std::cout << std::setw(12) << std::setprecision(1) << rss_mb;
        if (is_selected && color) {
            std::cout << "\033[0m";
        }
        std::cout << '\n';
    }

    std::cout.flush();
}

void rebuild_prev_map(const std::vector<ProcessSample>& curr, std::unordered_map<int, ProcessSample>& prev_map) {
    prev_map.clear();
    prev_map.reserve(curr.size());
    for (const auto& p : curr) {
        prev_map[p.pid] = p;
    }
}

std::optional<char> read_key_nonblocking() {
    char ch = 0;
    const ssize_t read_bytes = ::read(STDIN_FILENO, &ch, 1);
    if (read_bytes == 1) {
        return ch;
    }
    return std::nullopt;
}

int main(int argc, char** argv) {
    install_signal_handlers();

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

    if (!opts->watch) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opts->interval_ms));
        auto curr = reader.read_processes();
        const auto curr_time = std::chrono::steady_clock::now();
        const double elapsed_seconds = std::chrono::duration<double>(curr_time - prev_time).count();

        auto views = build_views(curr, prev_map, elapsed_seconds, clock_ticks);
        sort_views(views, opts->sort);
        print_table(views, opts->limit);
        return 0;
    }

    const bool interactive_tui = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    std::optional<TerminalRawMode> raw_mode;
    if (interactive_tui) {
        raw_mode.emplace();
    }

    std::size_t selected = 0;
    std::size_t top_index = 0;
    std::string status = "ready";
    std::vector<ProcessView> views;
    auto next_refresh = std::chrono::steady_clock::now();

    auto refresh_data = [&]() {
        auto curr = reader.read_processes();
        const auto curr_time = std::chrono::steady_clock::now();
        const double elapsed_seconds = std::chrono::duration<double>(curr_time - prev_time).count();

        views = build_views(curr, prev_map, elapsed_seconds, clock_ticks);
        sort_views(views, opts->sort);
        rebuild_prev_map(curr, prev_map);
        prev_time = curr_time;

        if (views.empty()) {
            selected = 0;
            top_index = 0;
        } else {
            if (selected >= views.size()) {
                selected = views.size() - 1;
            }
            if (selected < top_index) {
                top_index = selected;
            }
            if (selected >= top_index + opts->limit) {
                top_index = selected - opts->limit + 1;
            }
        }
    };

    refresh_data();
    draw_tui(views, selected, top_index, opts->limit, opts->interval_ms, opts->sort, status);
    next_refresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts->interval_ms);

    bool running = true;
    while (running && !g_should_exit) {
        bool redraw = false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh) {
            refresh_data();
            redraw = true;
            next_refresh = now + std::chrono::milliseconds(opts->interval_ms);
        }

        if (!interactive_tui) {
            if (redraw) {
                draw_tui(views, selected, top_index, opts->limit, opts->interval_ms, opts->sort, status);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        auto key = read_key_nonblocking();
        if (!key.has_value()) {
            if (redraw) {
                draw_tui(views, selected, top_index, opts->limit, opts->interval_ms, opts->sort, status);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            continue;
        }

        const char c = *key;
        if (c == 'q') {
            running = false;
            continue;
        }

        if (views.empty()) {
            status = "no processes";
            draw_tui(views, selected, top_index, opts->limit, opts->interval_ms, opts->sort, status);
            continue;
        }

        const int pid = views[selected].pid;
        if (c == 'j' && selected + 1 < views.size()) {
            ++selected;
            if (selected >= top_index + opts->limit) {
                top_index = selected - opts->limit + 1;
            }
            status = "selected pid " + std::to_string(views[selected].pid);
        } else if (c == 'k' && selected > 0) {
            --selected;
            if (selected < top_index) {
                top_index = selected;
            }
            status = "selected pid " + std::to_string(views[selected].pid);
        } else if (c == 's') {
            opts->sort = next_sort(opts->sort);
            sort_views(views, opts->sort);
            status = "sort changed to " + std::string(sort_name(opts->sort));
        } else if (c == 't') {
            status = execute_action(Action{ActionType::Kill, pid, std::nullopt});
        } else if (c == 'x') {
            status = execute_action(Action{ActionType::ForceKill, pid, std::nullopt});
        } else if (c == 'p') {
            status = execute_action(Action{ActionType::Stop, pid, std::nullopt});
        } else if (c == 'c') {
            status = execute_action(Action{ActionType::Continue, pid, std::nullopt});
        } else if (c == '+' || c == '-') {
            errno = 0;
            const int current_nice = getpriority(PRIO_PROCESS, pid);
            if (errno != 0) {
                status = "Не удалось прочитать nice: " + std::string(std::strerror(errno));
            } else {
                int target = current_nice + (c == '+' ? 1 : -1);
                target = std::max(-20, std::min(19, target));
                status = execute_action(Action{ActionType::Renice, pid, target});
            }
        }

        draw_tui(views, selected, top_index, opts->limit, opts->interval_ms, opts->sort, status);
    }

    if (isatty(STDOUT_FILENO)) {
        std::cout << "\033[2J\033[H";
    }
    return 0;
}
