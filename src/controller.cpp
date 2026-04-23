#include "controller.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <sys/resource.h>

namespace {
std::string errno_message(const char* prefix) {
    std::ostringstream out;
    out << prefix << ": " << std::strerror(errno);
    return out.str();
}
} // namespace

std::string execute_action(const Action& action) {
    if (action.type == ActionType::None) {
        return "Нет команды управления.";
    }
    if (!action.pid.has_value()) {
        return "Ошибка: не указан PID.";
    }

    const int pid = *action.pid;

    switch (action.type) {
    case ActionType::Kill:
        if (kill(pid, SIGTERM) != 0) {
            return errno_message("Не удалось отправить SIGTERM");
        }
        return "Отправлен SIGTERM процессу " + std::to_string(pid);
    case ActionType::ForceKill:
        if (kill(pid, SIGKILL) != 0) {
            return errno_message("Не удалось отправить SIGKILL");
        }
        return "Отправлен SIGKILL процессу " + std::to_string(pid);
    case ActionType::Stop:
        if (kill(pid, SIGSTOP) != 0) {
            return errno_message("Не удалось отправить SIGSTOP");
        }
        return "Отправлен SIGSTOP процессу " + std::to_string(pid);
    case ActionType::Continue:
        if (kill(pid, SIGCONT) != 0) {
            return errno_message("Не удалось отправить SIGCONT");
        }
        return "Отправлен SIGCONT процессу " + std::to_string(pid);
    case ActionType::Renice:
        if (!action.nice.has_value()) {
            return "Ошибка: не указано значение nice.";
        }
        if (setpriority(PRIO_PROCESS, pid, *action.nice) != 0) {
            return errno_message("Не удалось изменить nice");
        }
        return "Изменён nice процесса " + std::to_string(pid) + " на " + std::to_string(*action.nice);
    case ActionType::None:
        break;
    }

    return "Неизвестная команда.";
}
