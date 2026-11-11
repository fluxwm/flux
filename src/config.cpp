#include "flux/config.hpp"

#include <kdlpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace flux {

    namespace {

        constexpr char default_config_raw[] = {
#embed "../resources/default-config.kdl"
        };
        constexpr std::string_view default_config_kdl{default_config_raw, sizeof(default_config_raw)};

        std::string_view u8_view(std::u8string_view u8) noexcept {
            return {reinterpret_cast<const char*>(u8.data()), u8.size()};
        }

        std::string_view node_name(const kdl::Node& node) noexcept {
            return u8_view(node.name());
        }

        bool iequals(std::string_view a, std::string_view b) noexcept {
            return std::ranges::equal(a, b, [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }

        bool parse_combo(std::string_view combo, uint32_t& modifiers, xkb_keysym_t& keysym) {
            modifiers = 0;
            keysym = XKB_KEY_NoSymbol;
            std::string key_name;

            for (auto part : std::views::split(combo, '+')) {
                std::string_view token(part.begin(), part.end());
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
                    token.remove_prefix(1);
                }
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
                    token.remove_suffix(1);
                }

                if (token.empty()) {
                    continue;
                }

                if (iequals(token, "ctrl") || iequals(token, "control")) {
                    modifiers |= WLR_MODIFIER_CTRL;
                } else if (iequals(token, "alt") || iequals(token, "mod1")) {
                    modifiers |= WLR_MODIFIER_ALT;
                } else if (iequals(token, "super") || iequals(token, "mod") || iequals(token, "mod4") ||
                           iequals(token, "logo")) {
                    modifiers |= WLR_MODIFIER_LOGO;
                } else if (iequals(token, "shift")) {
                    modifiers |= WLR_MODIFIER_SHIFT;
                } else {
                    key_name = token;
                }
            }

            if (key_name.empty()) {
                return false;
            }

            keysym = xkb_keysym_from_name(key_name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
            return keysym != XKB_KEY_NoSymbol;
        }

        std::optional<int32_t> parse_int(const kdl::Node& node, int32_t min = 0) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty() || args[0].type() != kdl::Type::Number) {
                wlr_log(WLR_ERROR, "%.*s requires an integer argument", static_cast<int>(name.size()), name.data());
                return std::nullopt;
            }
            try {
                const int32_t val = args[0].as<int32_t>();
                if (val < min) {
                    wlr_log(WLR_ERROR, "invalid %.*s '%d' (must be >= %d)", static_cast<int>(name.size()), name.data(),
                            val, min);
                    return std::nullopt;
                }
                return val;
            } catch (const std::exception& e) {
                wlr_log(WLR_ERROR, "failed to parse %.*s: %s", static_cast<int>(name.size()), name.data(), e.what());
                return std::nullopt;
            }
        }

        std::optional<std::string> parse_string(const kdl::Node& node) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty() || args[0].type() != kdl::Type::String) {
                wlr_log(WLR_ERROR, "%.*s requires a string argument", static_cast<int>(name.size()), name.data());
                return std::nullopt;
            }
            return std::string(u8_view(args[0].as<std::u8string_view>()));
        }

        std::optional<Direction> parse_direction(const kdl::Node& node) {
            auto dir = parse_string(node);
            if (!dir) {
                return std::nullopt;
            }
            if (iequals(*dir, "left")) {
                return Direction::Left;
            }
            if (iequals(*dir, "right")) {
                return Direction::Right;
            }
            if (iequals(*dir, "up")) {
                return Direction::Up;
            }
            if (iequals(*dir, "down")) {
                return Direction::Down;
            }
            std::string_view name = node_name(node);
            wlr_log(WLR_ERROR, "invalid direction '%s' for %.*s", dir->c_str(), static_cast<int>(name.size()),
                    name.data());
            return std::nullopt;
        }

        std::optional<Action> parse_action(const kdl::Node& action_node) {
            std::string_view action_name = node_name(action_node);

            if (iequals(action_name, "quit")) {
                return action::Quit{};
            }
            if (iequals(action_name, "close-window")) {
                return action::CloseWindow{};
            }
            if (iequals(action_name, "cycle-window")) {
                return action::CycleWindow{};
            }
            if (iequals(action_name, "maximize-window")) {
                return action::MaximizeWindow{};
            }
            if (iequals(action_name, "fullscreen-window")) {
                return action::FullscreenWindow{};
            }
            if (iequals(action_name, "toggle-floating")) {
                return action::ToggleFloating{};
            }
            if (iequals(action_name, "focus-window")) {
                if (auto dir = parse_direction(action_node)) {
                    return action::FocusWindow{*dir};
                }
                return std::nullopt;
            }
            if (iequals(action_name, "focus-workspace")) {
                if (auto ws = parse_int(action_node, 1)) {
                    return action::FocusWorkspace{static_cast<uint32_t>(*ws)};
                }
                return std::nullopt;
            }
            if (iequals(action_name, "move-window-to-workspace")) {
                if (auto ws = parse_int(action_node, 1)) {
                    return action::MoveToWorkspace{static_cast<uint32_t>(*ws)};
                }
                return std::nullopt;
            }
            if (iequals(action_name, "spawn")) {
                auto program = parse_string(action_node);
                if (!program) {
                    return std::nullopt;
                }

                const auto& raw_args = action_node.args();
                std::vector<std::string> args;
                args.reserve(raw_args.size() - 1);
                for (size_t i = 1; i < raw_args.size(); ++i) {
                    if (raw_args[i].type() == kdl::Type::String) {
                        args.emplace_back(u8_view(raw_args[i].as<std::u8string_view>()));
                    }
                }

                return action::Spawn{
                    .program = std::move(*program),
                    .args = std::move(args),
                };
            }
            if (iequals(action_name, "spawn-sh")) {
                if (auto command = parse_string(action_node)) {
                    return action::Spawn{
                        .program = "/bin/sh",
                        .args = {"-c", std::move(*command)},
                    };
                }
                return std::nullopt;
            }

            return std::nullopt;
        }

        void parse_binds(std::vector<Keybind>& keybinds, const kdl::Node& binds_node) {
            for (const auto& bind_node : binds_node.children()) {
                std::string combo(node_name(bind_node));
                uint32_t modifiers = 0;
                xkb_keysym_t keysym = XKB_KEY_NoSymbol;

                if (!parse_combo(combo, modifiers, keysym)) {
                    wlr_log(WLR_ERROR, "invalid key combo in config: '%s'", combo.c_str());
                    continue;
                }

                std::vector<Action> actions;
                for (const auto& action_node : bind_node.children()) {
                    auto action_opt = parse_action(action_node);
                    if (!action_opt) {
                        std::string_view aname = node_name(action_node);
                        wlr_log(WLR_ERROR, "unknown or invalid action '%.*s' for bind '%s'",
                                static_cast<int>(aname.size()), aname.data(), combo.c_str());
                        continue;
                    }

                    actions.push_back(std::move(*action_opt));
                }

                if (!actions.empty()) {
                    keybinds.push_back(Keybind{
                        .modifiers = modifiers,
                        .keysym = keysym,
                        .actions = std::move(actions),
                    });
                }
            }
        }

        std::optional<bool> parse_bool(const kdl::Node& node) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty()) {
                return true;
            }
            if (args[0].type() == kdl::Type::Bool) {
                return args[0].as<bool>();
            }
            wlr_log(WLR_ERROR, "%.*s requires a boolean or no argument", static_cast<int>(name.size()), name.data());
            return std::nullopt;
        }

        void parse_keyboard(KeyboardConfig& keyboard, const kdl::Node& keyboard_node) {
            for (const auto& child : keyboard_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "repeat-rate")) {
                    if (auto val = parse_int(child)) {
                        keyboard.repeat_rate = *val;
                    }
                } else if (iequals(name, "repeat-delay")) {
                    if (auto val = parse_int(child)) {
                        keyboard.repeat_delay = *val;
                    }
                } else if (iequals(name, "numlock")) {
                    if (auto val = parse_bool(child)) {
                        keyboard.numlock = *val;
                    }
                } else if (iequals(name, "layout")) {
                    if (auto val = parse_string(child)) {
                        keyboard.layout = std::move(*val);
                    }
                } else if (iequals(name, "variant")) {
                    if (auto val = parse_string(child)) {
                        keyboard.variant = std::move(*val);
                    }
                } else if (iequals(name, "options")) {
                    if (auto val = parse_string(child)) {
                        keyboard.options = std::move(*val);
                    }
                } else if (iequals(name, "model")) {
                    if (auto val = parse_string(child)) {
                        keyboard.model = std::move(*val);
                    }
                } else if (iequals(name, "rules")) {
                    if (auto val = parse_string(child)) {
                        keyboard.rules = std::move(*val);
                    }
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in keyboard config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_touchpad(TouchpadConfig& touchpad, const kdl::Node& touchpad_node) {
            for (const auto& child : touchpad_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "tap-to-click")) {
                    if (auto val = parse_bool(child)) {
                        touchpad.tap_to_click = *val;
                    }
                } else if (iequals(name, "natural-scroll")) {
                    if (auto val = parse_bool(child)) {
                        touchpad.natural_scroll = *val;
                    }
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in touchpad config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_input(KeyboardConfig& keyboard, TouchpadConfig& touchpad, bool& middle_click_paste,
                         const kdl::Node& input_node) {
            for (const auto& child : input_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "keyboard")) {
                    parse_keyboard(keyboard, child);
                } else if (iequals(name, "touchpad")) {
                    parse_touchpad(touchpad, child);
                } else if (iequals(name, "middle-click-paste")) {
                    if (auto val = parse_bool(child)) {
                        middle_click_paste = *val;
                    }
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in input config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_cursor(CursorConfig& cursor, const kdl::Node& cursor_node) {
            for (const auto& child : cursor_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "theme")) {
                    if (auto val = parse_string(child)) {
                        cursor.theme = std::move(*val);
                    }
                } else if (iequals(name, "size")) {
                    if (auto val = parse_int(child, 1)) {
                        cursor.size = static_cast<uint32_t>(*val);
                    }
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in cursor config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_gestures(GesturesConfig& gestures, const kdl::Node& gestures_node) {
            for (const auto& child : gestures_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "workspace-swipe")) {
                    if (auto val = parse_bool(child)) {
                        gestures.workspace_swipe = *val;
                    }
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in gestures config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_gaps(GapsConfig& gaps, const kdl::Node& gaps_node) {
            for (const auto& child : gaps_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "inner")) {
                    if (auto val = parse_int(child)) {
                        gaps.inner = *val;
                    }
                } else if (iequals(name, "outer")) {
                    if (auto val = parse_int(child)) {
                        gaps.outer = *val;
                    }
                } else if (iequals(name, "smart")) {
                    if (auto val = parse_bool(child)) {
                        gaps.smart = *val;
                    }
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in gaps config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        std::optional<Layout> parse_layout_mode(const kdl::Node& node) {
            auto mode = parse_string(node);
            if (!mode) {
                return std::nullopt;
            }
            if (iequals(*mode, "master")) {
                return Layout::Master;
            }
            if (iequals(*mode, "monocle")) {
                return Layout::Monocle;
            }
            std::string_view name = node_name(node);
            wlr_log(WLR_ERROR, "invalid layout mode '%s' for %.*s", mode->c_str(), static_cast<int>(name.size()),
                    name.data());
            return std::nullopt;
        }

        void parse_layout(LayoutConfig& layout, const kdl::Node& layout_node) {
            for (const auto& child : layout_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "mode")) {
                    if (auto val = parse_layout_mode(child)) {
                        layout.mode = *val;
                    }
                } else if (iequals(name, "gaps")) {
                    parse_gaps(layout.gaps, child);
                } else {
                    wlr_log(WLR_ERROR, "unknown section '%.*s' in layout config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_environment(std::vector<EnvironmentVariable>& environment, const kdl::Node& environment_node) {
            for (const auto& child : environment_node.children()) {
                std::string_view name = node_name(child);
                if (name.empty() || name.contains('=')) {
                    wlr_log(WLR_ERROR, "invalid environment variable name '%.*s'", static_cast<int>(name.size()),
                            name.data());
                    continue;
                }

                const auto& args = child.args();
                std::optional<std::string> value;
                if (!args.empty() && args[0].type() != kdl::Type::Null) {
                    value = parse_string(child);
                    if (!value) {
                        continue;
                    }
                }

                auto it = std::ranges::find_if(environment,
                                               [name](const EnvironmentVariable& var) { return var.name == name; });
                if (it != environment.end()) {
                    it->value = std::move(value);
                } else {
                    environment.push_back(EnvironmentVariable{.name = std::string(name), .value = std::move(value)});
                }
            }
        }

        void parse_autostart(std::vector<Action>& autostart, const kdl::Node& autostart_node) {
            for (const auto& action_node : autostart_node.children()) {
                auto action_opt = parse_action(action_node);
                if (!action_opt) {
                    std::string_view aname = node_name(action_node);
                    wlr_log(WLR_ERROR, "unknown or invalid action '%.*s' in autostart", static_cast<int>(aname.size()),
                            aname.data());
                    continue;
                }

                autostart.push_back(std::move(*action_opt));
            }
        }

    }

    Config::Config(wl_event_loop* event_loop, std::optional<std::filesystem::path> custom_path)
        : m_path(resolve_path(custom_path)) {
        std::error_code ec;
        const bool has_file =
            custom_path.has_value() || std::filesystem::exists(m_path, ec) || generate_default_config();
        if (!has_file || !reload()) {
            load_from_string(default_config_kdl, "built-in defaults");
        }
        setup_watcher(event_loop);
    }

    Config::~Config() {
        if (m_inotify_source) {
            wl_event_source_remove(m_inotify_source);
        }
        if (m_inotify_fd >= 0) {
            close(m_inotify_fd);
        }
    }

    std::filesystem::path Config::resolve_path(const std::optional<std::filesystem::path>& custom_path) {
        std::filesystem::path path;
        if (custom_path.has_value()) {
            path = *custom_path;
        } else if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config && *xdg_config != '\0') {
            path = std::filesystem::path(xdg_config) / "flux" / "config.kdl";
        } else if (const char* home = std::getenv("HOME"); home && *home != '\0') {
            path = std::filesystem::path(home) / ".config" / "flux" / "config.kdl";
        }

        if (!path.empty()) {
            path = std::filesystem::absolute(path);
        }
        return path;
    }

    bool Config::generate_default_config() {
        if (m_path.empty()) {
            return false;
        }

        if (const auto parent_dir = m_path.parent_path(); !parent_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent_dir, ec);
            if (ec) {
                wlr_log(WLR_ERROR, "failed to create config directory '%s': %s", parent_dir.c_str(),
                        ec.message().c_str());
                return false;
            }
        }

        std::ofstream file(m_path, std::ios::binary);
        if (!file.is_open()) {
            wlr_log(WLR_ERROR, "failed to write default config to '%s'", m_path.c_str());
            return false;
        }

        file.write(default_config_kdl.data(), static_cast<std::streamsize>(default_config_kdl.size()));
        if (file.fail()) {
            wlr_log(WLR_ERROR, "error writing default config to '%s'", m_path.c_str());
            return false;
        }

        wlr_log(WLR_INFO, "generated default config at '%s'", m_path.c_str());
        return true;
    }

    void Config::update_watches() {
        if (m_inotify_fd < 0 || m_path.empty()) {
            return;
        }

        constexpr uint32_t watch_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
        const auto parent_dir = m_path.parent_path();
        if (!parent_dir.empty() && inotify_add_watch(m_inotify_fd, parent_dir.c_str(), watch_mask) < 0) {
            if (const auto grandparent = parent_dir.parent_path(); !grandparent.empty()) {
                inotify_add_watch(m_inotify_fd, grandparent.c_str(), watch_mask);
            }
        }

        m_real_filename.clear();
        std::error_code ec;
        const auto real_path = std::filesystem::canonical(m_path, ec);
        if (!ec && real_path != m_path) {
            if (real_path.filename() != m_path.filename()) {
                m_real_filename = real_path.filename();
            }
            if (real_path.has_parent_path() && real_path.parent_path() != parent_dir) {
                inotify_add_watch(m_inotify_fd, real_path.parent_path().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
            }
        }
    }

    void Config::setup_watcher(wl_event_loop* event_loop) {
        if (!event_loop || m_path.empty()) {
            return;
        }

        m_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotify_fd < 0) {
            return;
        }

        update_watches();

        auto on_inotify = [](int fd, uint32_t, void* data) {
            auto* self = static_cast<Config*>(data);
            const auto target_filename = self->m_path.filename();
            const auto parent_dirname = self->m_path.parent_path().filename();
            alignas(inotify_event) std::array<char, 4096> buf{};
            bool should_reload = false;
            bool dir_created = false;

            while (true) {
                const ssize_t len = read(fd, buf.data(), buf.size());
                if (len <= 0) {
                    break;
                }
                for (ssize_t i = 0; i + static_cast<ssize_t>(sizeof(inotify_event)) <= len;) {
                    const auto* ev = reinterpret_cast<const inotify_event*>(&buf[static_cast<size_t>(i)]);
                    if (ev->len > 0) {
                        const bool is_write_or_move = (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0;
                        if (target_filename == ev->name ||
                            (!self->m_real_filename.empty() && self->m_real_filename == ev->name)) {
                            std::error_code ec;
                            if (is_write_or_move || std::filesystem::is_symlink(self->m_path, ec)) {
                                should_reload = true;
                            }
                        } else if ((ev->mask & IN_ISDIR) && parent_dirname == ev->name) {
                            dir_created = true;
                        }
                    }
                    i += static_cast<ssize_t>(sizeof(inotify_event)) + ev->len;
                }
            }

            std::error_code ec;
            if (should_reload || (dir_created && std::filesystem::exists(self->m_path, ec))) {
                self->reload();
            } else if (dir_created) {
                self->update_watches();
            }
            return 0;
        };

        m_inotify_source = wl_event_loop_add_fd(event_loop, m_inotify_fd, WL_EVENT_READABLE, on_inotify, this);
    }

    bool Config::load_from_string(std::string_view content, const char* source_name) {
        std::u8string_view u8view(reinterpret_cast<const char8_t*>(content.data()), content.size());

        kdl::Document doc;
        try {
            doc = kdl::parse(u8view);
        } catch (const kdl::ParseError& e) {
            wlr_log(WLR_ERROR, "kdl parse error in '%s': %s", source_name, e.what());
            return false;
        } catch (const std::exception& e) {
            wlr_log(WLR_ERROR, "error reading kdl config '%s': %s", source_name, e.what());
            return false;
        }

        bool middle_click_paste = true;
        KeyboardConfig keyboard;
        TouchpadConfig touchpad;
        GesturesConfig gestures;
        CursorConfig cursor;
        LayoutConfig layout;
        std::vector<EnvironmentVariable> environment;
        std::vector<Action> autostart;
        std::vector<Keybind> keybinds;

        for (const auto& node : doc.nodes()) {
            std::string_view name = node_name(node);
            if (iequals(name, "autostart")) {
                parse_autostart(autostart, node);
            } else if (iequals(name, "input")) {
                parse_input(keyboard, touchpad, middle_click_paste, node);
            } else if (iequals(name, "gestures")) {
                parse_gestures(gestures, node);
            } else if (iequals(name, "cursor")) {
                parse_cursor(cursor, node);
            } else if (iequals(name, "layout")) {
                parse_layout(layout, node);
            } else if (iequals(name, "environment")) {
                parse_environment(environment, node);
            } else if (iequals(name, "binds")) {
                parse_binds(keybinds, node);
            } else {
                wlr_log(WLR_ERROR, "unknown section '%.*s' in config", static_cast<int>(name.size()), name.data());
            }
        }

        m_middle_click_paste = middle_click_paste;
        m_keyboard = std::move(keyboard);
        m_touchpad = touchpad;
        m_gestures = gestures;
        m_cursor = std::move(cursor);
        m_layout = layout;
        m_environment = std::move(environment);
        m_autostart = std::move(autostart);
        m_keybinds = std::move(keybinds);
        wlr_log(WLR_INFO, "loaded config from '%s'", source_name);

        if (m_on_reload) {
            m_on_reload();
        }
        return true;
    }

    bool Config::reload() {
        update_watches();

        std::error_code ec;
        if (m_path.empty() || !std::filesystem::exists(m_path, ec)) {
            wlr_log(WLR_ERROR, "config file does not exist: %s", m_path.c_str());
            return false;
        }

        std::ifstream file(m_path, std::ios::binary);
        if (!file.is_open()) {
            wlr_log(WLR_ERROR, "failed to open config file: %s", m_path.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return load_from_string(content, m_path.c_str());
    }

}
