#pragma once

#include "flux/wlroots.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flux {

    struct KeyboardConfig {
        int32_t repeat_rate = 25;
        int32_t repeat_delay = 600;
        bool numlock = false;
    };

    struct CursorConfig {
        std::string theme;
        uint32_t size = 24;

        bool operator==(const CursorConfig&) const = default;
    };

    enum class Direction : uint8_t {
        Left = WLR_DIRECTION_LEFT,
        Right = WLR_DIRECTION_RIGHT,
        Up = WLR_DIRECTION_UP,
        Down = WLR_DIRECTION_DOWN,
    };

    namespace action {
        struct Quit {};
        struct CloseWindow {};
        struct CycleWindow {};
        struct MaximizeWindow {};
        struct FullscreenWindow {};
        struct FocusWindow {
            Direction direction = Direction::Right;
        };

        struct Spawn {
            std::string program;
            std::vector<std::string> args;
        };

        struct FocusWorkspace {
            uint32_t workspace = 1;
        };

        struct MoveToWorkspace {
            uint32_t workspace = 1;
        };
    }

    using Action = std::variant<action::Quit, action::CloseWindow, action::CycleWindow, action::MaximizeWindow,
                                action::FullscreenWindow, action::FocusWindow, action::Spawn, action::FocusWorkspace,
                                action::MoveToWorkspace>;

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

        [[nodiscard]] const KeyboardConfig& keyboard() const noexcept { return m_keyboard; }
        [[nodiscard]] const CursorConfig& cursor() const noexcept { return m_cursor; }
        [[nodiscard]] std::span<const Action> autostart() const noexcept { return m_autostart; }
        [[nodiscard]] std::span<const Keybind> keybinds() const noexcept { return m_keybinds; }
        [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

        void set_on_reload(std::function<void()> callback) { m_on_reload = std::move(callback); }
        bool reload();

    private:
        static std::filesystem::path resolve_path(const std::optional<std::filesystem::path>& custom_path);
        void setup_watcher(wl_event_loop* event_loop);
        void update_watches();
        bool load_from_string(std::string_view content, const char* source_name);
        bool generate_default_config();

        std::filesystem::path m_path;
        std::filesystem::path m_real_filename;
        int m_inotify_fd = -1;
        wl_event_source* m_inotify_source = nullptr;
        std::function<void()> m_on_reload;

        KeyboardConfig m_keyboard;
        CursorConfig m_cursor;
        std::vector<Action> m_autostart;
        std::vector<Keybind> m_keybinds;
    };

}
