#pragma once

#include "flux/wlroots.hpp"

#include <vector>

namespace flux {

    template <typename Container, typename T>
    constexpr void erase_unique_ptr(Container& container, const T* target) noexcept {
        std::erase_if(container, [target](const auto& item) { return item.get() == target; });
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
