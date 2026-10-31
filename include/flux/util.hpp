#pragma once

#include "flux/wlroots.hpp"

#include <algorithm>
#include <vector>

namespace flux {

    template <typename Container, typename T>
    constexpr bool erase_unique_ptr(Container& container, const T* target) noexcept {
        return std::erase_if(container, [target](const auto& item) { return item.get() == target; }) > 0;
    }

    [[nodiscard]] constexpr wlr_box tile_box(const wlr_box& usable, size_t index, size_t total) noexcept {
        const int usable_w = std::max(1, usable.width);
        const int usable_h = std::max(1, usable.height);
        if (total <= 1) {
            return {.x = usable.x, .y = usable.y, .width = usable_w, .height = usable_h};
        }

        const int master_w = std::max(1, usable_w / 2);
        if (index == 0) {
            return {.x = usable.x, .y = usable.y, .width = master_w, .height = usable_h};
        }

        const int stack_w = std::max(1, usable_w - master_w);
        const size_t stack_count = total - 1;
        const int stack_h = std::max(1, usable_h / static_cast<int>(stack_count));
        const int y = usable.y + (static_cast<int>(index - 1) * stack_h);
        const int height = (index == total - 1) ? std::max(1, usable.y + usable_h - y) : stack_h;

        return {.x = usable.x + master_w, .y = y, .width = stack_w, .height = height};
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
