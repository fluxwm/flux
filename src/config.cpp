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

        std::string_view node_name(const kdl::Node& node) noexcept {
            return {reinterpret_cast<const char*>(node.name().data()), node.name().size()};
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
            if (iequals(action_name, "spawn")) {
                const auto& raw_args = action_node.args();
                if (raw_args.empty() || raw_args[0].type() != kdl::Type::String) {
                    return std::nullopt;
                }

                auto u8_prog = raw_args[0].as<std::u8string>();
                std::string program(u8_prog.begin(), u8_prog.end());

                std::vector<std::string> args;
                args.reserve(raw_args.size() - 1);
                for (size_t i = 1; i < raw_args.size(); ++i) {
                    if (raw_args[i].type() == kdl::Type::String) {
                        auto u8_arg = raw_args[i].as<std::u8string>();
                        args.emplace_back(u8_arg.begin(), u8_arg.end());
                    }
                }

                return action::Spawn{
                    .program = std::move(program),
                    .args = std::move(args),
                };
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
                } else {
                    wlr_log(WLR_ERROR, "unknown setting '%.*s' in keyboard config", static_cast<int>(name.size()),
                            name.data());
                }
            }
        }

        void parse_input(KeyboardConfig& keyboard, const kdl::Node& input_node) {
            for (const auto& child : input_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "keyboard")) {
                    parse_keyboard(keyboard, child);
                } else {
                    wlr_log(WLR_ERROR, "unknown section '%.*s' in input config", static_cast<int>(name.size()),
                            name.data());
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
        if (custom_path.has_value() || (!m_path.empty() && std::filesystem::exists(m_path))) {
            reload();
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

    void Config::setup_watcher(wl_event_loop* event_loop) {
        if (!event_loop || m_path.empty()) {
            return;
        }

        const auto parent_dir = m_path.parent_path();
        if (parent_dir.empty() || !std::filesystem::exists(parent_dir)) {
            return;
        }

        m_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotify_fd < 0) {
            return;
        }

        if (inotify_add_watch(m_inotify_fd, parent_dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
            close(m_inotify_fd);
            m_inotify_fd = -1;
            return;
        }

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

        auto on_inotify = [](int fd, uint32_t, void* data) {
            auto* self = static_cast<Config*>(data);
            const auto target_filename = self->m_path.filename();
            alignas(inotify_event) std::array<char, 4096> buf{};
            bool should_reload = false;

            while (true) {
                const ssize_t len = read(fd, buf.data(), buf.size());
                if (len <= 0) {
                    break;
                }
                for (ssize_t i = 0; i + static_cast<ssize_t>(sizeof(inotify_event)) <= len;) {
                    const auto* ev = reinterpret_cast<const inotify_event*>(&buf[static_cast<size_t>(i)]);
                    if (ev->len > 0 && (target_filename == ev->name ||
                                        (!self->m_real_filename.empty() && self->m_real_filename == ev->name))) {
                        should_reload = true;
                    }
                    i += static_cast<ssize_t>(sizeof(inotify_event)) + ev->len;
                }
            }

            if (should_reload) {
                self->reload();
            }
            return 0;
        };

        m_inotify_source = wl_event_loop_add_fd(event_loop, m_inotify_fd, WL_EVENT_READABLE, on_inotify, this);
    }

    bool Config::reload() {
        if (m_path.empty() || !std::filesystem::exists(m_path)) {
            wlr_log(WLR_ERROR, "config file does not exist: %s", m_path.c_str());
            return false;
        }

        std::ifstream file(m_path, std::ios::binary);
        if (!file.is_open()) {
            wlr_log(WLR_ERROR, "failed to open config file: %s", m_path.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::u8string_view u8view(reinterpret_cast<const char8_t*>(content.data()), content.size());

        kdl::Document doc;
        try {
            doc = kdl::parse(u8view);
        } catch (const kdl::ParseError& e) {
            wlr_log(WLR_ERROR, "kdl parse error in '%s': %s", m_path.c_str(), e.what());
            return false;
        } catch (const std::exception& e) {
            wlr_log(WLR_ERROR, "error reading kdl config '%s': %s", m_path.c_str(), e.what());
            return false;
        }

        KeyboardConfig keyboard;
        std::vector<Action> autostart;
        std::vector<Keybind> keybinds;

        for (const auto& node : doc.nodes()) {
            std::string_view name = node_name(node);
            if (iequals(name, "input")) {
                parse_input(keyboard, node);
            } else if (iequals(name, "autostart")) {
                parse_autostart(autostart, node);
            } else if (iequals(name, "binds")) {
                parse_binds(keybinds, node);
            } else {
                wlr_log(WLR_ERROR, "unknown section '%.*s' in config", static_cast<int>(name.size()), name.data());
            }
        }

        m_keyboard = keyboard;
        m_autostart = std::move(autostart);
        m_keybinds = std::move(keybinds);
        wlr_log(WLR_INFO, "loaded config from '%s'", m_path.c_str());

        if (m_on_reload) {
            m_on_reload();
        }
        return true;
    }

}
