#pragma once

#include "flux/config.hpp"
#include "flux/wlroots.hpp"

#include <algorithm>
#include <vector>

namespace flux {

    template <typename Container, typename T>
    constexpr bool erase_unique_ptr(Container& container, const T* target) noexcept {
        return std::erase_if(container, [target](const auto& item) { return item.get() == target; }) > 0;
    }

    [[nodiscard]] constexpr wlr_box tile_box(const wlr_box& usable, size_t index, size_t total,
                                             const LayoutConfig& layout) noexcept {
        if (layout.mode == Layout::Monocle) {
            total = 1;
        }

        const auto& gaps = layout.gaps;

        const int outer = (total <= 1 && gaps.smart) ? 0 : gaps.outer;
        const int area_x = usable.x + outer;
        const int area_y = usable.y + outer;
        const int area_w = std::max(1, usable.width - (2 * outer));
        const int area_h = std::max(1, usable.height - (2 * outer));
        wlr_box cell{};
        if (total <= 1) {
            cell = {.x = area_x, .y = area_y, .width = area_w, .height = area_h};
        } else {
            const int avail_w = std::max(1, area_w - gaps.inner);
            const int master_w = std::max(1, avail_w / 2);
            if (index == 0) {
                cell = {.x = area_x, .y = area_y, .width = master_w, .height = area_h};
            } else {
                const int stack_x = area_x + master_w + gaps.inner;
                const int stack_w = std::max(1, avail_w - master_w);
                const size_t stack_count = total - 1;
                const int avail_h = std::max(1, area_h - (static_cast<int>(stack_count - 1) * gaps.inner));
                const int stack_h = std::max(1, avail_h / static_cast<int>(stack_count));
                const int y = area_y + (static_cast<int>(index - 1) * (stack_h + gaps.inner));
                const int height = (index == total - 1) ? std::max(1, (area_y + area_h) - y) : stack_h;
                cell = {.x = stack_x, .y = y, .width = stack_w, .height = height};
            }
        }

        const bool smart_border = (total <= 1 && gaps.smart);
        const int bw = smart_border ? 0 : layout.border.width;
        return {
            .x = cell.x + bw,
            .y = cell.y + bw,
            .width = std::max(1, cell.width - (2 * bw)),
            .height = std::max(1, cell.height - (2 * bw)),
        };
    }

    [[nodiscard]] inline wlr_surface* root_popup_parent(wlr_surface* surface) noexcept {
        wlr_surface* root = surface ? wlr_surface_get_root_surface(surface) : nullptr;
        while (auto* xdg_surf = root ? wlr_xdg_surface_try_from_wlr_surface(root) : nullptr) {
            if (xdg_surf->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg_surf->popup && xdg_surf->popup->parent) {
                root = wlr_surface_get_root_surface(xdg_surf->popup->parent);
            } else {
                break;
            }
        }
        return root;
    }

}
