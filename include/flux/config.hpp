#pragma once

#include "flux/wlroots.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace flux {

    namespace action {
        struct Quit {};
        struct CloseWindow {};
        struct CycleWindow {};
        struct MaximizeWindow {};
        struct FullscreenWindow {};

        struct Spawn {
            std::string program;
            std::vector<std::string> args;
        };
    }

    using Action = std::variant<action::Quit, action::CloseWindow, action::CycleWindow, action::MaximizeWindow,
                                action::FullscreenWindow, action::Spawn>;

    struct Keybind {
        uint32_t modifiers = 0;
        xkb_keysym_t keysym = XKB_KEY_NoSymbol;
        std::vector<Action> actions;
    };

    class Config {
    public:
        explicit Config(wl_event_loop* event_loop, std::optional<std::filesystem::path> custom_path = std::nullopt);
        ~Config();

        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

        [[nodiscard]] std::span<const Action> autostart() const noexcept { return m_autostart; }
        [[nodiscard]] std::span<const Keybind> keybinds() const noexcept { return m_keybinds; }
        [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

        bool reload();

    private:
        static std::filesystem::path resolve_path(const std::optional<std::filesystem::path>& custom_path);
        void setup_watcher(wl_event_loop* event_loop);

        std::filesystem::path m_path;
        std::filesystem::path m_real_filename;
        int m_inotify_fd = -1;
        wl_event_source* m_inotify_source = nullptr;

        std::vector<Action> m_autostart;
        std::vector<Keybind> m_keybinds;
    };

}
