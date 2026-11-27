#include "frost/config.hpp"
#include "frost/log.hpp"

#include <kdlpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace frost {

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

                if (token.empty() || !key_name.empty()) {
                    return false;
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

            keysym = xkb_keysym_to_lower(xkb_keysym_from_name(key_name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE));
            return keysym != XKB_KEY_NoSymbol;
        }

        bool parse_int(const kdl::Node& node, int32_t& out, int32_t min = 0) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty() || args[0].type() != kdl::Type::Number) {
                log_error("{} requires an integer argument", name);
                return false;
            }
            try {
                const int32_t val = args[0].as<int32_t>();
                if (val < min) {
                    log_error("invalid {} '{}' (must be >= {})", name, val, min);
                    return false;
                }
                out = val;
                return true;
            } catch (const std::exception& e) {
                log_error("failed to parse {}: {}", name, e.what());
                return false;
            }
        }

        bool parse_int(const kdl::Node& node, uint32_t& out, int32_t min = 0) {
            int32_t val = 0;
            if (parse_int(node, val, min)) {
                out = static_cast<uint32_t>(val);
                return true;
            }
            return false;
        }

        bool parse_double(const kdl::Node& node, double& out, double min, double max) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty() || args[0].type() != kdl::Type::Number) {
                log_error("{} requires a number argument", name);
                return false;
            }
            try {
                const double val = args[0].as<double>();
                if (!std::isfinite(val) || val < min || val > max) {
                    log_error("invalid {} '{:g}' (must be between {:g} and {:g})", name, val, min, max);
                    return false;
                }
                out = val;
                return true;
            } catch (const std::exception& e) {
                log_error("failed to parse {}: {}", name, e.what());
                return false;
            }
        }

        std::optional<std::string> parse_string(const kdl::Node& node) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty() || args[0].type() != kdl::Type::String) {
                log_error("{} requires a string argument", name);
                return std::nullopt;
            }
            return std::string(u8_view(args[0].as<std::u8string_view>()));
        }

        void parse_string(const kdl::Node& node, std::string& out) {
            if (auto val = parse_string(node)) {
                out = std::move(*val);
            }
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
            log_error("invalid direction '{}' for {}", *dir, name);
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
            if (iequals(action_name, "swap-window")) {
                if (auto dir = parse_direction(action_node)) {
                    return action::SwapWindow{*dir};
                }
                return std::nullopt;
            }
            if (iequals(action_name, "focus-workspace")) {
                uint32_t ws = 0;
                if (parse_int(action_node, ws, 1)) {
                    return action::FocusWorkspace{ws};
                }
                return std::nullopt;
            }
            if (iequals(action_name, "move-window-to-workspace")) {
                uint32_t ws = 0;
                if (parse_int(action_node, ws, 1)) {
                    return action::MoveToWorkspace{ws};
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
                    } else {
                        log_error("spawn argument {} for '{}' must be a string", i, *program);
                        return std::nullopt;
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
                const std::string_view combo = node_name(bind_node);
                uint32_t modifiers = 0;
                xkb_keysym_t keysym = XKB_KEY_NoSymbol;

                if (!parse_combo(combo, modifiers, keysym)) {
                    log_error("invalid key combo in config: '{}'", combo);
                    continue;
                }

                bool repeat = false;
                bool locked = false;

                for (const auto& arg : bind_node.args()) {
                    if (arg.type() != kdl::Type::String) {
                        log_error("invalid argument on bind '{}'", combo);
                        continue;
                    }
                    std::string_view flag = u8_view(arg.as<std::u8string_view>());
                    if (iequals(flag, "repeat")) {
                        repeat = true;
                    } else if (iequals(flag, "locked")) {
                        locked = true;
                    } else {
                        log_error("unknown flag '{}' on bind '{}'", flag, combo);
                    }
                }

                std::vector<Action> actions;
                for (const auto& action_node : bind_node.children()) {
                    auto action_opt = parse_action(action_node);
                    if (!action_opt) {
                        std::string_view aname = node_name(action_node);
                        log_error("unknown or invalid action '{}' for bind '{}'", aname, combo);
                        continue;
                    }

                    actions.push_back(std::move(*action_opt));
                }

                if (!actions.empty()) {
                    keybinds.push_back(Keybind{
                        .modifiers = modifiers,
                        .keysym = keysym,
                        .actions = std::move(actions),
                        .repeat = repeat,
                        .locked = locked,
                    });
                }
            }
        }

        void parse_bool(const kdl::Node& node, bool& out) {
            std::string_view name = node_name(node);
            const auto& args = node.args();
            if (args.empty()) {
                out = true;
                return;
            }
            if (args[0].type() == kdl::Type::Bool) {
                out = args[0].as<bool>();
                return;
            }
            log_error("{} requires a boolean or no argument", name);
        }

        void parse_keyboard(KeyboardConfig& keyboard, const kdl::Node& keyboard_node) {
            for (const auto& child : keyboard_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "repeat-rate")) {
                    parse_int(child, keyboard.repeat_rate);
                } else if (iequals(name, "repeat-delay")) {
                    parse_int(child, keyboard.repeat_delay, 1);
                } else if (iequals(name, "numlock")) {
                    parse_bool(child, keyboard.numlock);
                } else if (iequals(name, "layout")) {
                    parse_string(child, keyboard.layout);
                } else if (iequals(name, "variant")) {
                    parse_string(child, keyboard.variant);
                } else if (iequals(name, "options")) {
                    parse_string(child, keyboard.options);
                } else if (iequals(name, "model")) {
                    parse_string(child, keyboard.model);
                } else if (iequals(name, "rules")) {
                    parse_string(child, keyboard.rules);
                } else {
                    log_error("unknown setting '{}' in keyboard config", name);
                }
            }
        }

        void parse_mouse(MouseConfig& mouse, const kdl::Node& mouse_node) {
            for (const auto& child : mouse_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "natural-scroll")) {
                    parse_bool(child, mouse.natural_scroll);
                } else {
                    log_error("unknown setting '{}' in mouse config", name);
                }
            }
        }

        void parse_touchpad(TouchpadConfig& touchpad, const kdl::Node& touchpad_node) {
            for (const auto& child : touchpad_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "tap-to-click")) {
                    parse_bool(child, touchpad.tap_to_click);
                } else if (iequals(name, "natural-scroll")) {
                    parse_bool(child, touchpad.natural_scroll);
                } else {
                    log_error("unknown setting '{}' in touchpad config", name);
                }
            }
        }

        void parse_input(KeyboardConfig& keyboard, MouseConfig& mouse, TouchpadConfig& touchpad,
                         bool& middle_click_paste, const kdl::Node& input_node) {
            for (const auto& child : input_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "keyboard")) {
                    parse_keyboard(keyboard, child);
                } else if (iequals(name, "mouse")) {
                    parse_mouse(mouse, child);
                } else if (iequals(name, "touchpad")) {
                    parse_touchpad(touchpad, child);
                } else if (iequals(name, "middle-click-paste")) {
                    parse_bool(child, middle_click_paste);
                } else {
                    log_error("unknown setting '{}' in input config", name);
                }
            }
        }

        void parse_output_mode(const kdl::Node& node, std::optional<OutputMode>& out) {
            if (!node.properties().empty()) {
                log_error("unexpected property on output mode (custom modes are not supported)");
                return;
            }
            auto str_opt = parse_string(node);
            if (!str_opt) {
                return;
            }
            std::string_view sv = *str_opt;
            const size_t x_pos = sv.find_first_of("xX");
            if (x_pos == std::string_view::npos || x_pos == 0 || x_pos + 1 >= sv.size()) {
                log_error("invalid output mode '{}' (expected WxH or WxH@Hz)", *str_opt);
                return;
            }

            const std::string_view w_str = sv.substr(0, x_pos);
            const std::string_view rest = sv.substr(x_pos + 1);
            const size_t at_pos = rest.find('@');
            const std::string_view h_str = (at_pos == std::string_view::npos) ? rest : rest.substr(0, at_pos);

            int32_t width = 0;
            int32_t height = 0;
            const auto [w_ptr, w_ec] = std::from_chars(w_str.data(), w_str.data() + w_str.size(), width);
            const auto [h_ptr, h_ec] = std::from_chars(h_str.data(), h_str.data() + h_str.size(), height);
            if (w_ec != std::errc{} || w_ptr != w_str.data() + w_str.size() || width <= 0 || h_ec != std::errc{} ||
                h_ptr != h_str.data() + h_str.size() || height <= 0) {
                log_error("invalid output mode '{}' (expected positive WxH)", *str_opt);
                return;
            }

            std::optional<int32_t> refresh_mhz;
            if (at_pos != std::string_view::npos) {
                const std::string_view hz_str = rest.substr(at_pos + 1);
                double hz = 0.0;
                const auto [hz_ptr, hz_ec] = std::from_chars(hz_str.data(), hz_str.data() + hz_str.size(), hz);
                if (hz_ec != std::errc{} || hz_ptr != hz_str.data() + hz_str.size() || !std::isfinite(hz) || hz < 1.0 ||
                    hz > 10000.0) {
                    log_error("invalid refresh rate '{}' in output mode '{}'", hz_str, *str_opt);
                    return;
                }
                refresh_mhz = static_cast<int32_t>(std::lround(hz * 1000.0));
            }

            out = OutputMode{.width = width, .height = height, .refresh_mhz = refresh_mhz};
        }

        void parse_output_transform(const kdl::Node& node, wl_output_transform& out) {
            auto str_opt = parse_string(node);
            if (!str_opt) {
                return;
            }
            constexpr std::array<std::pair<std::string_view, wl_output_transform>, 8> table{
                {
                    {"normal", WL_OUTPUT_TRANSFORM_NORMAL},
                    {"90", WL_OUTPUT_TRANSFORM_90},
                    {"180", WL_OUTPUT_TRANSFORM_180},
                    {"270", WL_OUTPUT_TRANSFORM_270},
                    {"flipped", WL_OUTPUT_TRANSFORM_FLIPPED},
                    {"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90},
                    {"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
                    {"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
                },
            };
            for (const auto& [name, transform] : table) {
                if (iequals(*str_opt, name)) {
                    out = transform;
                    return;
                }
            }
            log_error("invalid output transform '{}'", *str_opt);
        }

        void parse_output_position(const kdl::Node& node, std::optional<OutputPosition>& out) {
            const auto& args = node.args();
            if (args.size() != 2 || !node.properties().empty() || args[0].type() != kdl::Type::Number ||
                args[1].type() != kdl::Type::Number) {
                log_error("output position requires two integer arguments (x y)");
                return;
            }
            try {
                const auto n0 = args[0].as<kdl::Number>();
                const auto n1 = args[1].as<kdl::Number>();
                if (n0.representation() != kdl::NumberRepresentation::Integer ||
                    n1.representation() != kdl::NumberRepresentation::Integer) {
                    log_error("output position requires two integer arguments (x y)");
                    return;
                }
                const long long x = n0.as<long long>();
                const long long y = n1.as<long long>();
                constexpr auto min_val = static_cast<long long>(std::numeric_limits<int32_t>::min());
                constexpr auto max_val = static_cast<long long>(std::numeric_limits<int32_t>::max());
                if (x < min_val || x > max_val || y < min_val || y > max_val) {
                    log_error("output position coordinates out of 32-bit range");
                    return;
                }
                out = OutputPosition{.x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y)};
            } catch (const std::exception& e) {
                log_error("failed to parse output position: {}", e.what());
            }
        }

        void parse_output(std::vector<OutputConfig>& outputs, const kdl::Node& output_node) {
            auto name_opt = parse_string(output_node);
            if (!name_opt || name_opt->empty()) {
                if (name_opt) {
                    log_error("output section requires a non-empty name");
                }
                return;
            }

            OutputConfig cfg;
            cfg.name = std::move(*name_opt);
            for (const auto& child : output_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "off")) {
                    bool off = !cfg.enabled;
                    parse_bool(child, off);
                    cfg.enabled = !off;
                } else if (iequals(name, "mode")) {
                    parse_output_mode(child, cfg.mode);
                } else if (iequals(name, "scale")) {
                    double scale = 1.0;
                    if (parse_double(child, scale, 0.1, 10.0)) {
                        cfg.scale = static_cast<float>(scale);
                    }
                } else if (iequals(name, "transform")) {
                    parse_output_transform(child, cfg.transform);
                } else if (iequals(name, "position")) {
                    parse_output_position(child, cfg.position);
                } else {
                    log_error("unknown setting '{}' in output '{}' config", name, cfg.name);
                }
            }

            auto it = std::ranges::find_if(outputs, [&](const OutputConfig& c) { return iequals(c.name, cfg.name); });
            if (it != outputs.end()) {
                *it = std::move(cfg);
            } else {
                outputs.push_back(std::move(cfg));
            }
        }

        std::string output_description(const wlr_output* output) {
            std::string desc;
            for (const char* part : {output->make, output->model, output->serial}) {
                if (part && *part != '\0') {
                    if (!desc.empty()) {
                        desc.push_back(' ');
                    }
                    desc.append(part);
                }
            }
            return desc;
        }

        void parse_cursor(CursorConfig& cursor, const kdl::Node& cursor_node) {
            for (const auto& child : cursor_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "theme")) {
                    parse_string(child, cursor.theme);
                } else if (iequals(name, "size")) {
                    parse_int(child, cursor.size, 1);
                } else {
                    log_error("unknown setting '{}' in cursor config", name);
                }
            }
        }

        void parse_gestures(GesturesConfig& gestures, const kdl::Node& gestures_node) {
            for (const auto& child : gestures_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "workspace-swipe")) {
                    parse_bool(child, gestures.workspace_swipe);
                } else {
                    log_error("unknown setting '{}' in gestures config", name);
                }
            }
        }

        void parse_gaps(GapsConfig& gaps, const kdl::Node& gaps_node) {
            for (const auto& child : gaps_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "inner")) {
                    parse_int(child, gaps.inner);
                } else if (iequals(name, "outer")) {
                    parse_int(child, gaps.outer);
                } else if (iequals(name, "smart")) {
                    parse_bool(child, gaps.smart);
                } else {
                    log_error("unknown setting '{}' in gaps config", name);
                }
            }
        }

        void parse_color(const kdl::Node& node, std::array<float, 4>& out) {
            auto str_opt = parse_string(node);
            if (!str_opt) {
                return;
            }
            std::string_view str = *str_opt;
            if (iequals(str, "none")) {
                out = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
                return;
            }
            if (str.starts_with('#')) {
                str.remove_prefix(1);
            }
            uint32_t val = 0;
            if (str.size() == 6 || str.size() == 8) {
                const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val, 16);
                if (ec == std::errc{} && ptr == str.data() + str.size()) {
                    if (str.size() == 6) {
                        val = (val << 8) | 0xFF;
                    }
                    const float a = static_cast<float>(val & 0xFF) / 255.0f;
                    out = std::array<float, 4>{
                        (static_cast<float>((val >> 24) & 0xFF) / 255.0f) * a,
                        (static_cast<float>((val >> 16) & 0xFF) / 255.0f) * a,
                        (static_cast<float>((val >> 8) & 0xFF) / 255.0f) * a,
                        a,
                    };
                    return;
                }
            }
            std::string_view name = node_name(node);
            log_error("invalid color '{}' for {}", *str_opt, name);
        }

        void parse_border(BorderConfig& border, const kdl::Node& border_node) {
            for (const auto& child : border_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "width")) {
                    parse_int(child, border.width);
                } else if (iequals(name, "active-color")) {
                    parse_color(child, border.active_color);
                } else if (iequals(name, "inactive-color")) {
                    parse_color(child, border.inactive_color);
                } else {
                    log_error("unknown setting '{}' in border config", name);
                }
            }
        }

        void parse_layout_mode(const kdl::Node& node, Layout& out) {
            auto mode = parse_string(node);
            if (!mode) {
                return;
            }
            if (iequals(*mode, "master")) {
                out = Layout::Master;
                return;
            }
            if (iequals(*mode, "monocle")) {
                out = Layout::Monocle;
                return;
            }
            std::string_view name = node_name(node);
            log_error("invalid layout mode '{}' for {}", *mode, name);
        }

        void parse_layout(LayoutConfig& layout, const kdl::Node& layout_node) {
            for (const auto& child : layout_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "mode")) {
                    parse_layout_mode(child, layout.mode);
                } else if (iequals(name, "gaps")) {
                    parse_gaps(layout.gaps, child);
                } else if (iequals(name, "border")) {
                    parse_border(layout.border, child);
                } else {
                    log_error("unknown section '{}' in layout config", name);
                }
            }
        }

        void parse_animations(AnimationConfig& animations, const kdl::Node& animations_node) {
            for (const auto& child : animations_node.children()) {
                std::string_view name = node_name(child);
                if (iequals(name, "off")) {
                    bool off = !animations.enabled;
                    parse_bool(child, off);
                    animations.enabled = !off;
                } else if (iequals(name, "slowdown")) {
                    parse_double(child, animations.slowdown, 0.001, 100.0);
                } else {
                    log_error("unknown setting '{}' in animations config", name);
                }
            }
        }

        void parse_environment(std::vector<EnvironmentVariable>& environment, const kdl::Node& environment_node) {
            for (const auto& child : environment_node.children()) {
                std::string_view name = node_name(child);
                if (name.empty() || name.contains('=')) {
                    log_error("invalid environment variable name '{}'", name);
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
                    log_error("unknown or invalid action '{}' in autostart", aname);
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
            log_error("falling back to built-in default config");
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
            path = std::filesystem::path(xdg_config) / "frost" / "config.kdl";
        } else if (const char* home = std::getenv("HOME"); home && *home != '\0') {
            path = std::filesystem::path(home) / ".config" / "frost" / "config.kdl";
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
                log_error("failed to create config directory '{}': {}", parent_dir.c_str(), ec.message());
                return false;
            }
        }

        std::ofstream file(m_path, std::ios::binary);
        if (!file.is_open()) {
            log_error("failed to write default config to '{}'", m_path.c_str());
            return false;
        }

        file.write(default_config_kdl.data(), static_cast<std::streamsize>(default_config_kdl.size()));
        if (file.fail()) {
            log_error("error writing default config to '{}'", m_path.c_str());
            return false;
        }

        log_info("generated default config at '{}'", m_path.c_str());
        return true;
    }

    void Config::update_watches() {
        if (m_inotify_fd < 0 || m_path.empty()) {
            return;
        }

        for (int wd : m_watch_descriptors) {
            inotify_rm_watch(m_inotify_fd, wd);
        }
        m_watch_descriptors.clear();

        auto add_watch = [this](const std::filesystem::path& p, uint32_t mask) {
            if (const int wd = inotify_add_watch(m_inotify_fd, p.c_str(), mask); wd >= 0) {
                m_watch_descriptors.push_back(wd);
                return true;
            }
            return false;
        };

        constexpr uint32_t watch_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
        const auto parent_dir = m_path.parent_path();
        if (!parent_dir.empty() && !add_watch(parent_dir, watch_mask)) {
            if (const auto grandparent = parent_dir.parent_path(); !grandparent.empty()) {
                add_watch(grandparent, watch_mask);
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
                add_watch(real_path.parent_path(), IN_CLOSE_WRITE | IN_MOVED_TO);
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
            log_error("kdl parse error in '{}': {}", source_name, e.what());
            return false;
        } catch (const std::exception& e) {
            log_error("error reading kdl config '{}': {}", source_name, e.what());
            return false;
        }

        bool middle_click_paste = true;
        KeyboardConfig keyboard;
        MouseConfig mouse;
        TouchpadConfig touchpad;
        std::vector<OutputConfig> outputs;
        GesturesConfig gestures;
        CursorConfig cursor;
        LayoutConfig layout;
        AnimationConfig animations;
        std::vector<EnvironmentVariable> environment;
        std::vector<Action> autostart;
        std::vector<Keybind> keybinds;

        for (const auto& node : doc.nodes()) {
            std::string_view name = node_name(node);
            if (iequals(name, "autostart")) {
                parse_autostart(autostart, node);
            } else if (iequals(name, "input")) {
                parse_input(keyboard, mouse, touchpad, middle_click_paste, node);
            } else if (iequals(name, "output")) {
                parse_output(outputs, node);
            } else if (iequals(name, "gestures")) {
                parse_gestures(gestures, node);
            } else if (iequals(name, "cursor")) {
                parse_cursor(cursor, node);
            } else if (iequals(name, "layout")) {
                parse_layout(layout, node);
            } else if (iequals(name, "animations")) {
                parse_animations(animations, node);
            } else if (iequals(name, "environment")) {
                parse_environment(environment, node);
            } else if (iequals(name, "binds")) {
                parse_binds(keybinds, node);
            } else {
                log_error("unknown section '{}' in config", name);
            }
        }

        m_middle_click_paste = middle_click_paste;
        m_keyboard = std::move(keyboard);
        m_mouse = mouse;
        m_touchpad = touchpad;
        m_outputs = std::move(outputs);
        m_gestures = gestures;
        m_cursor = std::move(cursor);
        m_layout = layout;
        m_animations = animations;
        m_environment = std::move(environment);
        m_autostart = std::move(autostart);
        m_keybinds = std::move(keybinds);
        log_info("loaded config from '{}'", source_name);

        if (m_on_reload) {
            m_on_reload();
        }
        return true;
    }

    bool Config::reload() {
        update_watches();

        std::error_code ec;
        if (m_path.empty() || !std::filesystem::exists(m_path, ec)) {
            log_error("config file does not exist: {}", m_path.c_str());
            return false;
        }

        std::ifstream file(m_path, std::ios::binary);
        if (!file.is_open()) {
            log_error("failed to open config file: {}", m_path.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return load_from_string(content, m_path.c_str());
    }

    const OutputConfig* Config::output_config(const wlr_output* output) const noexcept {
        if (!output) {
            return nullptr;
        }
        if (output->name && *output->name != '\0') {
            auto it = std::ranges::find_if(m_outputs, [name = std::string_view(output->name)](const OutputConfig& cfg) {
                return iequals(cfg.name, name);
            });
            if (it != m_outputs.end()) {
                return &*it;
            }
        }
        if (const std::string desc = output_description(output); !desc.empty()) {
            auto it =
                std::ranges::find_if(m_outputs, [&desc](const OutputConfig& cfg) { return iequals(cfg.name, desc); });
            if (it != m_outputs.end()) {
                return &*it;
            }
        }
        return nullptr;
    }

}
