#pragma once

#include "frost/config.hpp"
#include "frost/wlroots.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace frost {
    class Window;
}

namespace frost::layout {

    struct Context {
        std::span<Window* const> windows;
        std::span<Window* const> focus_stack;
        wlr_box usable{};
        const LayoutConfig& config;
    };

    struct WorkArea {
        wlr_box box{};
        int border_width = 0;
    };

    [[nodiscard]] constexpr WorkArea work_area(const wlr_box& usable, const LayoutConfig& config,
                                               bool solitary) noexcept {
        const int outer = (solitary && config.gaps.smart) ? 0 : config.gaps.outer;
        return {
            .box =
                {
                    .x = usable.x + outer,
                    .y = usable.y + outer,
                    .width = std::max(1, usable.width - (2 * outer)),
                    .height = std::max(1, usable.height - (2 * outer)),
                },
            .border_width = (solitary && config.gaps.smart) ? 0 : config.border.width,
        };
    }

    [[nodiscard]] constexpr wlr_box inset_border(const wlr_box& cell, int bw) noexcept {
        return {
            .x = cell.x + bw,
            .y = cell.y + bw,
            .width = std::max(1, cell.width - (2 * bw)),
            .height = std::max(1, cell.height - (2 * bw)),
        };
    }

    class ResizeSession {
    public:
        virtual ~ResizeSession() = default;

        ResizeSession(const ResizeSession&) = delete;
        ResizeSession& operator=(const ResizeSession&) = delete;
        ResizeSession(ResizeSession&&) = delete;
        ResizeSession& operator=(ResizeSession&&) = delete;

        [[nodiscard]] virtual uint32_t edges() const noexcept = 0;
        [[nodiscard]] virtual bool update(const Context& ctx, int dx, int dy) = 0;
        [[nodiscard]] virtual bool is_valid(const Context& ctx) const noexcept = 0;
        [[nodiscard]] virtual bool involves_window(const Window* window) const noexcept = 0;

    protected:
        ResizeSession() = default;
    };

    class Base {
    public:
        virtual ~Base() = default;

        Base(const Base&) = delete;
        Base& operator=(const Base&) = delete;
        Base(Base&&) = delete;
        Base& operator=(Base&&) = delete;

        [[nodiscard]] virtual Layout mode() const noexcept = 0;
        [[nodiscard]] virtual bool is_solitary(size_t tiled_count) const noexcept { return tiled_count <= 1; }
        [[nodiscard]] virtual bool hides_unfocused() const noexcept { return false; }
        [[nodiscard]] virtual bool can_resize(size_t tiled_count) const noexcept { return tiled_count > 1; }

        virtual void arrange(const Context& ctx) = 0;
        virtual void on_window_removed(const Window* /*window*/, size_t /*remaining_non_floating*/) noexcept {}
        virtual void on_windows_swapped(Window* /*a*/, Window* /*b*/) {}

        [[nodiscard]] virtual wlr_box next_tile_box(const Context& ctx) const noexcept = 0;
        [[nodiscard]] virtual Window* window_in_direction(const Context& ctx, const Window* focused, Direction dir,
                                                          bool has_adj_output) const noexcept = 0;
        [[nodiscard]] virtual Window* floating_fallback(const Context& /*ctx*/, Direction /*dir*/,
                                                        Window* top_tiled) const noexcept {
            return top_tiled;
        }
        [[nodiscard]] virtual Window* swap_target_in_direction(const Context& /*ctx*/, const Window* /*focused*/,
                                                               Direction /*dir*/) const noexcept {
            return nullptr;
        }
        [[nodiscard]] virtual Window* window_at(const Context& /*ctx*/, double /*x*/, double /*y*/) const noexcept {
            return nullptr;
        }
        [[nodiscard]] virtual std::unique_ptr<ResizeSession> begin_resize(const Context& /*ctx*/, Window* /*window*/,
                                                                          double /*cursor_x*/, double /*cursor_y*/) {
            return nullptr;
        }

    protected:
        Base() = default;
    };

    [[nodiscard]] std::unique_ptr<Base> create(Layout mode);

    namespace detail {
        [[nodiscard]] std::unique_ptr<Base> create_master();
        [[nodiscard]] std::unique_ptr<Base> create_monocle();
    }

}
