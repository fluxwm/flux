#pragma once

#include "flux/wlroots.hpp"

#include <algorithm>
#include <utility>

namespace flux {

    template <typename Container, typename T>
    constexpr bool erase_unique_ptr(Container& container, const T* target) noexcept {
        auto it = std::ranges::find_if(container, [target](const auto& item) { return item.get() == target; });
        if (it == container.end()) {
            return false;
        }
        auto removed = std::move(*it);
        container.erase(it);
        return true;
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
