#include "flux/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string_view>
#include <unistd.h>

namespace flux {

    namespace {
        using namespace std::chrono;

        wlr_log_importance s_verbosity = WLR_INFO;
        bool s_use_color = false;

        struct LevelStyle {
            std::string_view name;
            std::string_view color;
        };

        constexpr auto level_styles = std::to_array<LevelStyle>({
            {.name = "SILENT", .color = ""},
            {.name = "ERROR", .color = "\033[31m"},
            {.name = " INFO", .color = "\033[32m"},
            {.name = "DEBUG", .color = "\033[34m"},
        });

        std::string_view determine_target(std::string_view file_and_line, char (&out)[32]) {
            if (const auto src_pos = file_and_line.rfind("src/"); src_pos != std::string_view::npos) {
                auto file_part = file_and_line.substr(src_pos + 4);
                if (const auto slash_pos = file_part.rfind('/'); slash_pos != std::string_view::npos) {
                    file_part = file_part.substr(slash_pos + 1);
                }
                const auto stem_end = file_part.find_first_of(".:");
                const auto stem = file_part.substr(0, stem_end);
                if (stem == "main" || stem.empty()) {
                    return "flux";
                }
                const auto res = std::format_to_n(out, sizeof(out), "flux::{}", stem);
                return std::string_view(out, res.out);
            }
            return file_and_line.empty() ? "flux" : "wlroots";
        }

        void log_callback(wlr_log_importance importance, const char* fmt, va_list args) {
            if (importance > s_verbosity) {
                return;
            }

            char buffer[2048];
            const int len = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
            if (len <= 0) {
                return;
            }

            const size_t actual_len = std::min(static_cast<size_t>(len), sizeof(buffer) - 1);
            const std::string_view full_msg(buffer, actual_len);
            std::string_view file_and_line;
            std::string_view msg = full_msg;

            if (full_msg.starts_with('[')) {
                if (const auto close_bracket = full_msg.find("] "); close_bracket != std::string_view::npos) {
                    file_and_line = full_msg.substr(1, close_bracket - 1);
                    msg = full_msg.substr(close_bracket + 2);
                }
            }

            while (msg.ends_with('\n') || msg.ends_with('\r')) {
                msg.remove_suffix(1);
            }

            char target_buf[32];
            const std::string_view target = determine_target(file_and_line, target_buf);

            if (target == "wlroots" && s_verbosity < WLR_DEBUG && importance > WLR_ERROR) {
                return;
            }

            const auto now = system_clock::now();
            const auto local_now = [now] {
                try {
                    return current_zone()->to_local(now);
                } catch (...) {
                    return local_time<system_clock::duration>(now.time_since_epoch());
                }
            }();
            const auto time = floor<microseconds>(local_now);

            const auto idx = static_cast<size_t>(importance);
            const auto& style = idx < level_styles.size() ? level_styles[idx] : level_styles[2];

            if (s_use_color) {
                std::println(stderr, "\033[2m{:%T}\033[0m {}{}\033[0m \033[2m{}:\033[0m {}", time, style.color,
                             style.name, target, msg);
            } else {
                std::println(stderr, "{:%T} {} {}: {}", time, style.name, target, msg);
            }
        }
    }

    void log_init(wlr_log_importance importance) {
        s_verbosity = importance;
        const char* no_color = std::getenv("NO_COLOR");
        s_use_color = isatty(STDERR_FILENO) != 0 && (no_color == nullptr || no_color[0] == '\0');
        wlr_log_init(WLR_DEBUG, log_callback);
    }

}
