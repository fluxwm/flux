#include "frost/window/xwayland.hpp"
#include "frost/compositor.hpp"
#include "frost/workspace.hpp"

#include <algorithm>

namespace frost::window {

    XWayland::XWayland(Compositor& compositor, wlr_xwayland_surface* surface)
        : Window(compositor, surface->title, surface->_class), m_surface(surface) {
        attach_scene_tree(wlr_scene_tree_create(initial_parent_tree()));
        surface->data = m_scene_tree;

        m_listeners.listen(&surface->events.associate, this, &XWayland::on_associate);
        m_listeners.listen(&surface->events.dissociate, this, &XWayland::on_dissociate);
        m_listeners.listen(&surface->events.destroy, this, &XWayland::on_destroy);
        m_listeners.listen(&surface->events.request_configure, this, &XWayland::on_request_configure);
        m_listeners.listen(&surface->events.request_maximize, this, &XWayland::on_request_maximize);
        m_listeners.listen(&surface->events.request_fullscreen, this, &XWayland::on_request_fullscreen);
        m_listeners.listen(&surface->events.request_activate, this, &XWayland::on_request_activate);
        m_listeners.listen(&surface->events.request_close, [this](void*) { close(); });
        m_listeners.listen(&surface->events.set_title, this, &XWayland::on_set_title);
        m_listeners.listen(&surface->events.set_class, this, &XWayland::on_set_app_id);
        m_listeners.listen(&surface->events.set_parent, this, &XWayland::on_set_parent);
        m_listeners.listen(&surface->events.set_hints, this, &XWayland::on_set_hints);
        m_listeners.listen(&surface->events.set_override_redirect, this, &XWayland::on_set_override_redirect);

        if (surface->x != 0 || surface->y != 0) {
            m_animation.set_target(surface->x, surface->y);
            snap_animation();
        }

        if (surface->surface) {
            on_associate();
        }
    }

    XWayland::~XWayland() {
        m_surface_listeners.clear();
        m_listeners.clear();
        if (m_surface) {
            m_surface->data = nullptr;
        }
        if (m_scene_tree) {
            wlr_scene_node_destroy(&m_scene_tree->node);
            m_scene_tree = nullptr;
        }
    }

    wlr_surface* XWayland::surface() const noexcept {
        return m_surface->surface;
    }

    std::pair<int, int> XWayland::preferred_size() const noexcept {
        return {static_cast<int>(m_surface->width), static_cast<int>(m_surface->height)};
    }

    void XWayland::close() noexcept {
        wlr_xwayland_surface_close(m_surface);
    }

    void XWayland::apply_activated(bool activated) noexcept {
        wlr_xwayland_surface_activate(m_surface, activated);
        if (activated && m_surface->minimized) {
            wlr_xwayland_surface_set_minimized(m_surface, false);
        }
    }

    void XWayland::apply_fullscreen(bool fullscreen) noexcept {
        wlr_xwayland_surface_set_fullscreen(m_surface, fullscreen);
    }

    void XWayland::apply_maximized(bool maximized) noexcept {
        wlr_xwayland_surface_set_maximized(m_surface, maximized, maximized);
    }

    void XWayland::configure_position(int x, int y) noexcept {
        if (is_mapped()) {
            const wlr_box box = geometry_box();
            wlr_xwayland_surface_configure(m_surface, static_cast<int16_t>(x), static_cast<int16_t>(y),
                                           static_cast<uint16_t>(std::max(1, box.width)),
                                           static_cast<uint16_t>(std::max(1, box.height)));
        }
    }

    bool XWayland::configure_geometry(const wlr_box& box) noexcept {
        const int w = std::max(1, box.width);
        const int h = std::max(1, box.height);
        wlr_xwayland_surface_configure(m_surface, static_cast<int16_t>(box.x), static_cast<int16_t>(box.y),
                                       static_cast<uint16_t>(w), static_cast<uint16_t>(h));
        return false;
    }

    void XWayland::apply_resize(int w, int h) noexcept {
        if (const auto* hints = m_surface->size_hints) {
            clamp_size(w, h, hints->min_width, hints->min_height, hints->max_width, hints->max_height);
        }
        int new_x = m_grab_box.x;
        int new_y = m_grab_box.y;
        if (m_resize_edges & WLR_EDGE_LEFT) {
            new_x += (m_grab_box.width - w);
        }
        if (m_resize_edges & WLR_EDGE_TOP) {
            new_y += (m_grab_box.height - h);
        }
        set_geometry({.x = new_x, .y = new_y, .width = w, .height = h});
    }

    Window* XWayland::query_parent() const noexcept {
        return Window::try_from(m_surface->parent);
    }

    bool XWayland::wants_floating() const noexcept {
        if (m_surface->modal || m_surface->parent != nullptr) {
            return true;
        }
        const bool is_floating_type =
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP) ||
            wlr_xwayland_surface_has_window_type(m_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION);
        if (is_floating_type) {
            return true;
        }
        if (const auto* hints = m_surface->size_hints) {
            if ((hints->max_width > 0 && hints->min_width == hints->max_width) ||
                (hints->max_height > 0 && hints->min_height == hints->max_height)) {
                return true;
            }
        }
        return false;
    }

    void XWayland::on_associate() {
        on_dissociate();
        if (m_surface->surface) {
            m_subsurface_tree = wlr_scene_tree_create(m_scene_tree);
            if (m_subsurface_tree) {
                wlr_scene_subsurface_tree_create(m_subsurface_tree, m_surface->surface);
            }
            m_surface_listeners.listen(&m_surface->surface->events.map, this, &XWayland::on_map);
            m_surface_listeners.listen(&m_surface->surface->events.unmap, this, &XWayland::on_unmap);
            m_surface_listeners.listen(&m_surface->surface->events.commit, this, &XWayland::on_commit);
            if (m_surface->surface->mapped) {
                on_map();
            }
        }
    }

    void XWayland::on_dissociate() {
        m_surface_listeners.clear();
        if (m_subsurface_tree) {
            wlr_scene_node_destroy(&m_subsurface_tree->node);
            m_subsurface_tree = nullptr;
        }
    }

    void XWayland::on_map() {
        m_floating = wants_floating();
        if (m_floating) {
            if (const auto* hints = m_surface->size_hints;
                hints && (hints->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION))) {
                m_placed = true;
            }
        }
        if (m_surface->fullscreen) {
            m_fullscreen = true;
            m_maximized = false;
            m_placed = false;
            wlr_xwayland_surface_set_fullscreen(m_surface, true);
        } else if (m_surface->maximized_horz && m_surface->maximized_vert) {
            m_maximized = true;
            m_placed = false;
        }
        wlr_xwayland_surface_set_maximized(m_surface, m_maximized, m_maximized);

        Window::on_map();
    }

    void XWayland::on_commit() {
        const int old_x = m_animation.base_x();
        const int old_y = m_animation.base_y();

        check_center();

        finish_commit(old_x, old_y);
    }

    void XWayland::on_request_configure(wlr_xwayland_surface_configure_event* event) {
        if (!is_mapped() || is_floating()) {
            m_animation.set_target(event->x, event->y);
            if (is_mapped()) {
                m_animation.set_has_initial_position(true);
            }
            snap_animation();
            wlr_xwayland_surface_configure(m_surface, event->x, event->y, event->width, event->height);
            update_borders();
        } else if (m_workspace) {
            m_workspace->arrange();
        }
    }

    void XWayland::on_request_activate() {
        if (is_mapped()) {
            m_compositor.focus_window(this);
        }
    }

    void XWayland::on_request_maximize() {
        if (!m_fullscreen && m_maximized != (m_surface->maximized_horz && m_surface->maximized_vert)) {
            toggle_maximize();
        } else {
            wlr_xwayland_surface_set_maximized(m_surface, m_maximized, m_maximized);
        }
    }

    void XWayland::on_request_fullscreen() {
        set_fullscreen(m_surface->fullscreen);
    }

    void XWayland::on_set_title() {
        update_title(m_surface->title);
    }

    void XWayland::on_set_app_id() {
        update_app_id(m_surface->_class);
    }

    void XWayland::on_set_hints() {
        if (!m_floating && !m_floating_override && wants_floating()) {
            set_floating(true);
        }
    }

    void XWayland::on_set_override_redirect() {
        if (m_surface->override_redirect) {
            m_compositor.recreate_as_popup(this);
        }
    }

}
