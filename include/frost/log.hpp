#pragma once

#include "frost/wlroots.hpp"

#include <concepts>
#include <format>
#include <source_location>
#include <string_view>
#include <type_traits>

namespace frost {

    void log_init(wlr_log_importance importance);

    namespace detail {

        [[nodiscard]] bool log_enabled(wlr_log_importance importance) noexcept;
        void log_emit(wlr_log_importance importance, std::source_location loc, std::string_view fmt,
                      std::format_args args);

        template <typename... Args>
        struct FormatWithLocation {
            std::format_string<Args...> fmt;
            std::source_location loc;

            template <typename T>
                requires std::convertible_to<const T&, std::string_view>
            consteval FormatWithLocation( // NOLINT(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
                const T& str, std::source_location location = std::source_location::current())
                : fmt(str), loc(location) {}
        };

    }

    template <typename... Args>
    void log_error(detail::FormatWithLocation<std::type_identity_t<Args>...> fmt_loc, const Args&... args) {
        if (detail::log_enabled(WLR_ERROR)) {
            detail::log_emit(WLR_ERROR, fmt_loc.loc, fmt_loc.fmt.get(), std::make_format_args(args...));
        }
    }

    template <typename... Args>
    void log_info(detail::FormatWithLocation<std::type_identity_t<Args>...> fmt_loc, const Args&... args) {
        if (detail::log_enabled(WLR_INFO)) {
            detail::log_emit(WLR_INFO, fmt_loc.loc, fmt_loc.fmt.get(), std::make_format_args(args...));
        }
    }

    template <typename... Args>
    void log_debug(detail::FormatWithLocation<std::type_identity_t<Args>...> fmt_loc, const Args&... args) {
        if (detail::log_enabled(WLR_DEBUG)) {
            detail::log_emit(WLR_DEBUG, fmt_loc.loc, fmt_loc.fmt.get(), std::make_format_args(args...));
        }
    }

}
