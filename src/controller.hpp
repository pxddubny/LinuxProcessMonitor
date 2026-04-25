#pragma once

#include <optional>
#include <string>

enum class ActionType {
    None,
    Kill,
    ForceKill,
    Stop,
    Continue,
    Renice,
};

struct Action {
    ActionType type{ActionType::None};
    std::optional<int> pid;
    std::optional<int> nice;
};

std::string execute_action(const Action& action);
