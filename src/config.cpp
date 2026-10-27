#include "flux/config.hpp"
#include "flux/wlroots.hpp"

#include <kdlpp.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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

        void parse_binds(Config& config, const kdl::Node& binds_node) {
            for (const auto& bind_node : binds_node.children()) {
                std::string combo(node_name(bind_node));
                uint32_t modifiers = 0;
                xkb_keysym_t keysym = XKB_KEY_NoSymbol;

                if (!parse_combo(combo, modifiers, keysym)) {
                    wlr_log(WLR_ERROR, "invalid key combo in config: '%s'", combo.c_str());
                    continue;
                }

                for (const auto& action_node : bind_node.children()) {
                    auto action_opt = parse_action(action_node);
                    if (!action_opt) {
                        std::string aname(node_name(action_node));
                        wlr_log(WLR_ERROR, "unknown or invalid action '%s' for bind '%s'", aname.c_str(),
                                combo.c_str());
                        continue;
                    }

                    config.keybinds.push_back(Keybind{
                        .modifiers = modifiers,
                        .keysym = keysym,
                        .action = std::move(*action_opt),
                    });
                }
            }
        }

        void parse_autostart(Config& config, const kdl::Node& autostart_node) {
            for (const auto& action_node : autostart_node.children()) {
                auto action_opt = parse_action(action_node);
                if (!action_opt) {
                    std::string aname(node_name(action_node));
                    wlr_log(WLR_ERROR, "unknown or invalid action '%s' in autostart", aname.c_str());
                    continue;
                }

                config.autostart.push_back(std::move(*action_opt));
            }
        }

    }

    std::optional<Config> Config::load(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            wlr_log(WLR_ERROR, "config file does not exist: %s", path.c_str());
            return std::nullopt;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            wlr_log(WLR_ERROR, "failed to open config file: %s", path.c_str());
            return std::nullopt;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::u8string_view u8view(reinterpret_cast<const char8_t*>(content.data()), content.size());

        kdl::Document doc;
        try {
            doc = kdl::parse(u8view);
        } catch (const kdl::ParseError& e) {
            wlr_log(WLR_ERROR, "kdl parse error in '%s': %s", path.c_str(), e.what());
            return std::nullopt;
        } catch (const std::exception& e) {
            wlr_log(WLR_ERROR, "error reading kdl config '%s': %s", path.c_str(), e.what());
            return std::nullopt;
        }

        Config config;

        for (const auto& node : doc.nodes()) {
            std::string_view name = node_name(node);
            if (iequals(name, "autostart")) {
                parse_autostart(config, node);
            } else if (iequals(name, "binds")) {
                parse_binds(config, node);
            }
        }

        wlr_log(WLR_INFO, "loaded config from '%s'", path.c_str());
        return config;
    }

}
