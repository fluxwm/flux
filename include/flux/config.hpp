#pragma once

#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace flux {

    namespace action {
        struct Quit {};
        struct CloseWindow {};
        struct CycleWindow {};
        struct MaximizeWindow {};

        struct Spawn {
            std::string program;
            std::vector<std::string> args;
        };
    }

    using Action =
        std::variant<action::Quit, action::CloseWindow, action::CycleWindow, action::MaximizeWindow, action::Spawn>;

    struct Keybind {
        uint32_t modifiers = 0;
        xkb_keysym_t keysym = XKB_KEY_NoSymbol;
        Action action;
    };

    struct Config {
        std::vector<Action> autostart;
        std::vector<Keybind> keybinds;

        static std::optional<Config> load(const std::filesystem::path& path);
    };

}
