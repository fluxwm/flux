#pragma once

#include "flux/wlroots.hpp"

#include <array>
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
        std::string layout;
        std::string variant;
        std::string options;
        std::string model;
        std::string rules;

        bool operator==(const KeyboardConfig&) const = default;
    };

    struct TouchpadConfig {
        bool tap_to_click = false;
        bool natural_scroll = false;
    };

    struct GesturesConfig {
        bool workspace_swipe = false;
    };

    struct CursorConfig {
        std::string theme;
        uint32_t size = 24;

        bool operator==(const CursorConfig&) const = default;
    };

    enum class Layout : uint8_t {
        Master,
        Monocle,
    };

    struct GapsConfig {
        int32_t inner = 0;
        int32_t outer = 0;
        bool smart = false;
    };

    struct BorderConfig {
        int32_t width = 0;
        std::array<float, 4> active_color{0.498f, 0.784f, 1.0f, 1.0f};
        std::array<float, 4> inactive_color{0.2f, 0.2f, 0.2f, 1.0f};
    };

    struct LayoutConfig {
        Layout mode = Layout::Master;
        GapsConfig gaps{};
        BorderConfig border{};
    };

    struct EnvironmentVariable {
        std::string name;
        std::optional<std::string> value;
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
        struct ToggleFloating {};
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
                                action::FullscreenWindow, action::ToggleFloating, action::FocusWindow, action::Spawn,
                                action::FocusWorkspace, action::MoveToWorkspace>;

    constexpr uint32_t key_modifier_mask =
        WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;

    struct Keybind {
        uint32_t modifiers = 0;
        xkb_keysym_t keysym = XKB_KEY_NoSymbol;
        std::vector<Action> actions;
        bool repeat = false;
        bool locked = false;
    };

    class Config {
    public:
        explicit Config(wl_event_loop* event_loop, std::optional<std::filesystem::path> custom_path = std::nullopt);
        ~Config();

        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

        [[nodiscard]] bool middle_click_paste() const noexcept { return m_middle_click_paste; }
        [[nodiscard]] const KeyboardConfig& keyboard() const noexcept { return m_keyboard; }
        [[nodiscard]] const TouchpadConfig& touchpad() const noexcept { return m_touchpad; }
        [[nodiscard]] const GesturesConfig& gestures() const noexcept { return m_gestures; }
        [[nodiscard]] const CursorConfig& cursor() const noexcept { return m_cursor; }
        [[nodiscard]] const LayoutConfig& layout() const noexcept { return m_layout; }
        [[nodiscard]] std::span<const EnvironmentVariable> environment() const noexcept { return m_environment; }
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
        std::vector<int> m_watch_descriptors;
        std::function<void()> m_on_reload;

        bool m_middle_click_paste = true;
        KeyboardConfig m_keyboard;
        TouchpadConfig m_touchpad;
        GesturesConfig m_gestures;
        CursorConfig m_cursor;
        LayoutConfig m_layout;
        std::vector<EnvironmentVariable> m_environment;
        std::vector<Action> m_autostart;
        std::vector<Keybind> m_keybinds;
    };

}
