#include "flux/window.hpp"
#include "flux/compositor.hpp"
#include "flux/output.hpp"
#include "flux/popup.hpp"
#include "flux/workspace.hpp"

#include <algorithm>

namespace flux {

    namespace {

        [[nodiscard]] Workspace* initial_workspace(Compositor& compositor) noexcept {
            Output* out = compositor.focused_output();
            return out ? &out->active_workspace() : nullptr;
        }

    }

    Window::Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel)
        : m_compositor(compositor), m_xdg_toplevel(xdg_toplevel), m_workspace(initial_workspace(compositor)),
          m_workspace_id(m_workspace ? m_workspace->id() : 1) {
        if (xdg_toplevel->title) {
            m_title = xdg_toplevel->title;
        }
        if (xdg_toplevel->app_id) {
            m_app_id = xdg_toplevel->app_id;
        }

        wlr_scene_tree* parent_tree = m_workspace ? m_workspace->tree() : compositor.scene().windows();
        m_scene_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_toplevel->base);
        m_scene_tree->node.data = this;
        xdg_toplevel->base->data = m_scene_tree;

        constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (auto& border : m_borders) {
            border = wlr_scene_rect_create(m_scene_tree, 0, 0, transparent);
            wlr_scene_node_set_enabled(&border->node, false);
        }

        m_listeners.listen(&xdg_toplevel->base->surface->events.map, this, &Window::on_map);
        m_listeners.listen(&xdg_toplevel->base->surface->events.unmap, this, &Window::on_unmap);
        m_listeners.listen(&xdg_toplevel->base->surface->events.commit, this, &Window::on_commit);
        m_listeners.listen(&xdg_toplevel->events.destroy, this, &Window::on_destroy);
        m_listeners.listen(&xdg_toplevel->events.request_maximize, this, &Window::on_request_maximize);
        m_listeners.listen(&xdg_toplevel->events.request_fullscreen, this, &Window::on_request_fullscreen);
        m_listeners.listen(&xdg_toplevel->events.set_title, this, &Window::on_set_title);
        m_listeners.listen(&xdg_toplevel->events.set_app_id, this, &Window::on_set_app_id);
        m_listeners.listen(&xdg_toplevel->events.set_parent, this, &Window::on_set_parent);
    }

    Window::Window(Compositor& compositor, wlr_xwayland_surface* xwayland_surface)
        : m_compositor(compositor), m_xwayland_surface(xwayland_surface), m_workspace(initial_workspace(compositor)),
          m_workspace_id(m_workspace ? m_workspace->id() : 1) {
        if (xwayland_surface->title) {
            m_title = xwayland_surface->title;
        }
        if (xwayland_surface->_class) {
            m_app_id = xwayland_surface->_class;
        }

        wlr_scene_tree* parent_tree = m_workspace ? m_workspace->tree() : compositor.scene().windows();
        m_scene_tree = wlr_scene_tree_create(parent_tree);
        m_scene_tree->node.data = this;
        xwayland_surface->data = m_scene_tree;

        constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (auto& border : m_borders) {
            border = wlr_scene_rect_create(m_scene_tree, 0, 0, transparent);
            wlr_scene_node_set_enabled(&border->node, false);
        }

        m_listeners.listen(&xwayland_surface->events.associate, this, &Window::on_associate);
        m_listeners.listen(&xwayland_surface->events.dissociate, this, &Window::on_dissociate);
        m_listeners.listen(&xwayland_surface->events.destroy, this, &Window::on_destroy);
        m_listeners.listen(&xwayland_surface->events.request_configure, this, &Window::on_request_configure);
        m_listeners.listen(&xwayland_surface->events.request_maximize, this, &Window::on_request_maximize);
        m_listeners.listen(&xwayland_surface->events.request_fullscreen, this, &Window::on_request_fullscreen);
        m_listeners.listen(&xwayland_surface->events.request_activate, this, &Window::on_request_activate);
        m_listeners.listen(&xwayland_surface->events.request_close, [this](void*) { close(); });
        m_listeners.listen(&xwayland_surface->events.set_title, this, &Window::on_set_title);
        m_listeners.listen(&xwayland_surface->events.set_class, this, &Window::on_set_app_id);
        m_listeners.listen(&xwayland_surface->events.set_parent, this, &Window::on_set_parent);
        m_listeners.listen(&xwayland_surface->events.set_hints, this, &Window::on_set_hints);
        m_listeners.listen(&xwayland_surface->events.set_override_redirect, this, &Window::on_set_override_redirect);

        if (xwayland_surface->x != 0 || xwayland_surface->y != 0) {
            move_scene_tree(xwayland_surface->x, xwayland_surface->y);
        }

        if (xwayland_surface->surface) {
            on_associate();
        }
    }

    Window::~Window() {
        m_surface_listeners.clear();
        m_decoration_listeners.clear();
        m_listeners.clear();
        if (m_ext_toplevel_handle) {
            wlr_ext_foreign_toplevel_handle_v1_destroy(m_ext_toplevel_handle);
            m_ext_toplevel_handle = nullptr;
        }
        if (m_xdg_toplevel && m_xdg_toplevel->base) {
            m_xdg_toplevel->base->data = nullptr;
        }
        if (m_xwayland_surface) {
            m_xwayland_surface->data = nullptr;
        }
        if (m_scene_tree) {
            m_scene_tree->node.data = nullptr;
            if (m_xwayland_surface) {
                wlr_scene_node_destroy(&m_scene_tree->node);
                m_scene_tree = nullptr;
            }
        }
    }

    bool Window::wants_floating() const noexcept {
        if (m_xdg_toplevel) {
            const auto& state = m_xdg_toplevel->current;
            return m_xdg_toplevel->parent != nullptr || (state.max_width > 0 && state.min_width == state.max_width) ||
                   (state.max_height > 0 && state.min_height == state.max_height);
        }
        if (m_xwayland_surface) {
            if (m_xwayland_surface->modal || m_xwayland_surface->parent != nullptr) {
                return true;
            }
            const bool is_floating_type =
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface,
                                                     WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP) ||
                wlr_xwayland_surface_has_window_type(m_xwayland_surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION);
            if (is_floating_type) {
                return true;
            }
            if (const auto* hints = m_xwayland_surface->size_hints) {
                if (hints->min_width > 0 && hints->min_height > 0 &&
                    (hints->min_width == hints->max_width || hints->min_height == hints->max_height)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    void Window::on_associate() {
        m_surface_listeners.clear();
        if (m_xwayland_surface && m_xwayland_surface->surface) {
            wlr_scene_subsurface_tree_create(m_scene_tree, m_xwayland_surface->surface);
            m_surface_listeners.listen(&m_xwayland_surface->surface->events.map, this, &Window::on_map);
            m_surface_listeners.listen(&m_xwayland_surface->surface->events.unmap, this, &Window::on_unmap);
            m_surface_listeners.listen(&m_xwayland_surface->surface->events.commit, this, &Window::on_commit);
            if (m_xwayland_surface->surface->mapped) {
                on_map();
            }
        }
    }

    void Window::on_dissociate() {
        m_surface_listeners.clear();
    }

    void Window::on_map() {
        wlr_log(WLR_DEBUG, "window mapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());

        if (m_xwayland_surface) {
            m_floating = wants_floating();
            if (m_floating) {
                if (const auto* hints = m_xwayland_surface->size_hints;
                    hints && (hints->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION))) {
                    m_placed = true;
                }
            }
            if (m_xwayland_surface->fullscreen) {
                m_fullscreen = true;
                m_maximized = false;
                m_placed = false;
                wlr_xwayland_surface_set_fullscreen(m_xwayland_surface, true);
            } else if (m_xwayland_surface->maximized_horz && m_xwayland_surface->maximized_vert) {
                m_maximized = true;
                m_placed = false;
            }
            wlr_xwayland_surface_set_maximized(m_xwayland_surface, m_maximized, m_maximized);
        }

        const wlr_ext_foreign_toplevel_handle_v1_state state{
            .title = m_title.c_str(),
            .app_id = m_app_id.c_str(),
        };
        m_ext_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(m_compositor.foreign_toplevel_list(), &state);

        m_compositor.map_window(this);
        update_borders();
    }

    void Window::on_unmap() {
        if (m_ext_toplevel_handle) {
            wlr_ext_foreign_toplevel_handle_v1_destroy(m_ext_toplevel_handle);
            m_ext_toplevel_handle = nullptr;
        }

        if (m_fullscreen) {
            m_fullscreen = false;
            reparent_to_workspace();
        }
        m_maximized = false;
        m_floating_override = false;
        m_placed = false;
        m_resize_edges = WLR_EDGE_NONE;
        m_floating_box = {};
        m_activated = false;
        update_borders();

        wlr_log(WLR_DEBUG, "window unmapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        m_compositor.unmap_window(this);
    }

    void Window::on_commit() {
        if (m_xdg_toplevel && m_xdg_toplevel->base->initial_commit) {
            on_request_decoration_mode();
            wlr_xdg_toplevel_set_wm_capabilities(m_xdg_toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
            m_floating = wants_floating();
            if (!m_floating) {
                wlr_xdg_toplevel_set_tiled(m_xdg_toplevel,
                                           WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
            }
            if (m_xdg_toplevel->requested.fullscreen) {
                set_fullscreen(true);
            } else if (!m_floating && m_workspace) {
                set_geometry(m_workspace->next_tile_box());
            } else {
                wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
            }
            return;
        }

        if (m_xdg_toplevel && m_resize_edges != WLR_EDGE_NONE) {
            if (is_floating()) {
                const wlr_box box = geometry_box();
                const int x =
                    (m_resize_edges & WLR_EDGE_LEFT) ? (m_grab_box.x + m_grab_box.width - box.width) : m_grab_box.x;
                const int y =
                    (m_resize_edges & WLR_EDGE_TOP) ? (m_grab_box.y + m_grab_box.height - box.height) : m_grab_box.y;
                set_position(x, y);
            }
            if (!m_resizing && !m_xdg_toplevel->current.resizing) {
                m_resize_edges = WLR_EDGE_NONE;
            }
        }

        if (!m_floating && !m_floating_override && wants_floating()) {
            on_set_parent();
        } else if (is_mapped() && is_floating() && !m_placed && m_workspace) {
            center(m_workspace->output().usable_area());
        }
        update_borders();
    }

    void Window::on_destroy() {
        m_compositor.remove_window(this);
    }

    void Window::on_request_configure(wlr_xwayland_surface_configure_event* event) {
        if (!is_mapped() || is_floating()) {
            move_scene_tree(event->x, event->y);
            wlr_xwayland_surface_configure(m_xwayland_surface, event->x, event->y, event->width, event->height);
            update_borders();
        } else if (m_workspace) {
            m_workspace->arrange();
            wlr_xwayland_surface_configure(m_xwayland_surface, m_xwayland_surface->x, m_xwayland_surface->y,
                                           m_xwayland_surface->width, m_xwayland_surface->height);
        }
    }

    void Window::on_request_activate() {
        if (is_mapped()) {
            m_compositor.focus_window(this);
        }
    }

    void Window::on_request_maximize() {
        if (m_xdg_toplevel) {
            if (m_xdg_toplevel->base->initialized) {
                wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
            }
        } else if (m_xwayland_surface) {
            if (!m_fullscreen &&
                m_maximized != (m_xwayland_surface->maximized_horz && m_xwayland_surface->maximized_vert)) {
                toggle_maximize();
            } else {
                wlr_xwayland_surface_set_maximized(m_xwayland_surface, m_maximized, m_maximized);
            }
        }
    }

    void Window::toggle_maximize() {
        if (m_fullscreen) {
            return;
        }

        if (m_xwayland_surface && is_floating()) {
            m_floating_box = geometry_box();
        }

        m_maximized = !m_maximized;
        m_placed = false;
        if (m_xdg_toplevel) {
            if (!m_maximized && m_floating) {
                wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
            }
        } else if (m_xwayland_surface) {
            wlr_xwayland_surface_set_maximized(m_xwayland_surface, m_maximized, m_maximized);
            restore_floating_box();
        }
        if (is_mapped()) {
            if (m_workspace) {
                m_workspace->arrange();
            }
            m_compositor.refocus();
        }
    }

    void Window::restore_floating_box() noexcept {
        if (!m_xwayland_surface || !is_mapped() || !is_floating()) {
            return;
        }
        if (m_floating_box.width <= 0 || m_floating_box.height <= 0) {
            if (!m_floating_override || !m_workspace) {
                return;
            }
            const wlr_box& usable = m_workspace->output().usable_area();
            const int w = std::max(1, (usable.width * 2) / 3);
            const int h = std::max(1, (usable.height * 2) / 3);
            m_floating_box = {
                .x = usable.x + ((usable.width - w) / 2),
                .y = usable.y + ((usable.height - h) / 2),
                .width = w,
                .height = h,
            };
        }
        m_placed = true;
        set_geometry(m_floating_box);
    }

    void Window::set_floating(bool floating) {
        if (!floating && m_xwayland_surface && is_floating()) {
            m_floating_box = geometry_box();
        }
        m_floating = floating;
        m_placed = false;
        if (m_xdg_toplevel && m_xdg_toplevel->base->initialized) {
            constexpr uint32_t edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
            wlr_xdg_toplevel_set_tiled(m_xdg_toplevel, m_floating ? 0 : edges);
            if (is_floating()) {
                wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
            }
        }
        if (is_mapped()) {
            restore_floating_box();
            if (m_workspace) {
                m_workspace->arrange();
            }
            m_compositor.refocus();
        }
        update_borders();
    }

    void Window::toggle_floating() {
        if (m_fullscreen) {
            return;
        }

        const bool floating = !is_floating();
        m_floating_override = true;
        if (m_maximized) {
            m_maximized = false;
            if (m_xwayland_surface) {
                wlr_xwayland_surface_set_maximized(m_xwayland_surface, false, false);
            }
        }
        set_floating(floating);
    }

    void Window::on_request_fullscreen() {
        if (m_xdg_toplevel) {
            if (m_xdg_toplevel->base->initialized) {
                set_fullscreen(m_xdg_toplevel->requested.fullscreen);
            }
        } else if (m_xwayland_surface) {
            set_fullscreen(m_xwayland_surface->fullscreen);
        }
    }

    void Window::set_fullscreen(bool fullscreen) {
        if (m_fullscreen == fullscreen) {
            if (m_xdg_toplevel && m_xdg_toplevel->base->initialized) {
                wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
            }
            return;
        }

        if (fullscreen && m_xwayland_surface && is_floating()) {
            m_floating_box = geometry_box();
        }

        m_fullscreen = fullscreen;
        m_placed = false;
        update_borders();

        if (m_fullscreen) {
            if (m_xdg_toplevel) {
                if (m_xdg_toplevel->requested.fullscreen_output) {
                    if (Output* out = m_compositor.output_from_wlr(m_xdg_toplevel->requested.fullscreen_output)) {
                        set_output(out);
                    }
                }
                wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, true);
            } else if (m_xwayland_surface) {
                wlr_xwayland_surface_set_fullscreen(m_xwayland_surface, true);
            }
        } else {
            reparent_to_workspace();
            if (m_xdg_toplevel) {
                wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, false);
                if (m_floating && !m_maximized) {
                    wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
                }
            } else if (m_xwayland_surface) {
                wlr_xwayland_surface_set_fullscreen(m_xwayland_surface, false);
                restore_floating_box();
            }
        }

        if (is_mapped()) {
            if (m_fullscreen && m_workspace) {
                m_workspace->promote(this);
            }
            if (m_workspace) {
                m_workspace->arrange();
            }
            m_compositor.refocus();
        } else if (m_fullscreen && m_workspace) {
            set_geometry(m_workspace->output().layout_box());
        }
    }

    void Window::set_decoration(wlr_xdg_toplevel_decoration_v1* decoration) {
        if (!m_xdg_toplevel) {
            return;
        }
        m_decoration = decoration;
        m_decoration_listeners.clear();
        m_decoration_listeners.listen(&decoration->events.request_mode, this, &Window::on_request_decoration_mode);
        m_decoration_listeners.listen(&decoration->events.destroy, this, &Window::on_destroy_decoration);
        on_request_decoration_mode();
    }

    void Window::on_request_decoration_mode() {
        if (m_decoration && m_xdg_toplevel && m_xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_decoration_v1_set_mode(m_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }

    void Window::on_destroy_decoration() {
        m_decoration_listeners.clear();
        m_decoration = nullptr;
    }

    bool Window::move_scene_tree(int x, int y) noexcept {
        if (m_scene_tree->node.x == x && m_scene_tree->node.y == y) {
            return false;
        }
        wlr_scene_node_set_position(&m_scene_tree->node, x, y);
        for (const auto& popup : m_compositor.popups()) {
            if (popup->root_tree() == m_scene_tree) {
                popup->update_position();
            }
        }
        return true;
    }

    void Window::set_position(int x, int y) noexcept {
        if (!move_scene_tree(x, y)) {
            return;
        }
        if (m_xwayland_surface && is_mapped()) {
            const wlr_box box = geometry_box();
            wlr_xwayland_surface_configure(m_xwayland_surface, static_cast<int16_t>(x), static_cast<int16_t>(y),
                                           static_cast<uint16_t>(std::max(1, box.width)),
                                           static_cast<uint16_t>(std::max(1, box.height)));
        }
    }

    void Window::set_geometry(const wlr_box& box) noexcept {
        move_scene_tree(box.x, box.y);
        const int w = std::max(1, box.width);
        const int h = std::max(1, box.height);
        if (m_xdg_toplevel) {
            wlr_xdg_toplevel_set_size(m_xdg_toplevel, w, h);
        } else if (m_xwayland_surface) {
            if (m_xwayland_surface->x != static_cast<int16_t>(box.x) ||
                m_xwayland_surface->y != static_cast<int16_t>(box.y) ||
                m_xwayland_surface->width != static_cast<uint16_t>(w) ||
                m_xwayland_surface->height != static_cast<uint16_t>(h) || !is_mapped()) {
                wlr_xwayland_surface_configure(m_xwayland_surface, static_cast<int16_t>(box.x),
                                               static_cast<int16_t>(box.y), static_cast<uint16_t>(w),
                                               static_cast<uint16_t>(h));
            }
        }
        update_borders();
    }

    void Window::set_activated(bool activated) noexcept {
        if (m_activated == activated) {
            return;
        }
        m_activated = activated;
        if (m_xdg_toplevel) {
            wlr_xdg_toplevel_set_activated(m_xdg_toplevel, activated);
        } else if (m_xwayland_surface) {
            wlr_xwayland_surface_activate(m_xwayland_surface, activated);
            if (activated) {
                if (m_xwayland_surface->minimized) {
                    wlr_xwayland_surface_set_minimized(m_xwayland_surface, false);
                }
                wlr_xwayland_surface_restack(m_xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
            }
        }
        update_borders();
    }

    void Window::update_borders() noexcept {
        const auto& layout_cfg = m_compositor.config().layout();
        const auto& border_cfg = layout_cfg.border;

        const bool smart_solitary =
            layout_cfg.gaps.smart && (m_maximized || (is_tiled() && (!m_workspace || m_workspace->is_solitary())));

        const auto& color = m_activated ? border_cfg.active_color : border_cfg.inactive_color;
        const bool visible = is_mapped() && !m_fullscreen && border_cfg.width > 0 && !smart_solitary && color[3] > 0.0f;
        if (!visible) {
            for (auto* border : m_borders) {
                wlr_scene_node_set_enabled(&border->node, false);
            }
            return;
        }

        const int bw = border_cfg.width;
        const wlr_box box = geometry_box();
        const int w = std::max(1, box.width);
        const int h = std::max(1, box.height);

        std::array<wlr_box, 4> rects = {
            wlr_box{.x = -bw, .y = -bw, .width = w + (2 * bw), .height = bw},
            wlr_box{.x = -bw, .y = h, .width = w + (2 * bw), .height = bw},
            wlr_box{.x = -bw, .y = 0, .width = bw, .height = h},
            wlr_box{.x = w, .y = 0, .width = bw, .height = h},
        };

        const bool clamp = m_workspace && m_workspace->output().animation().is_busy();
        const wlr_box clip = clamp ? m_workspace->output().layout_box() : wlr_box{};
        const int origin_x = clamp ? (m_workspace->tree()->node.x + m_scene_tree->node.x) : 0;
        const int origin_y = clamp ? m_scene_tree->node.y : 0;

        for (size_t i = 0; i < 4; ++i) {
            auto& r = rects[i];
            if (clamp) {
                const int x1 = std::max(origin_x + r.x, clip.x);
                const int y1 = std::max(origin_y + r.y, clip.y);
                const int x2 = std::min(origin_x + r.x + r.width, clip.x + clip.width);
                const int y2 = std::min(origin_y + r.y + r.height, clip.y + clip.height);
                if (x1 >= x2 || y1 >= y2) {
                    wlr_scene_node_set_enabled(&m_borders[i]->node, false);
                    continue;
                }
                r = {.x = x1 - origin_x, .y = y1 - origin_y, .width = x2 - x1, .height = y2 - y1};
            }

            wlr_scene_node_set_position(&m_borders[i]->node, r.x, r.y);
            wlr_scene_rect_set_size(m_borders[i], r.width, r.height);
            wlr_scene_rect_set_color(m_borders[i], color.data());
            wlr_scene_node_set_enabled(&m_borders[i]->node, true);
        }
    }

    wlr_box Window::geometry_box() const noexcept {
        if (m_xdg_toplevel) {
            const wlr_box& geo = m_xdg_toplevel->base->geometry;
            const auto* s = surface();
            return {
                .x = m_scene_tree->node.x,
                .y = m_scene_tree->node.y,
                .width = geo.width > 0 ? geo.width : (s ? s->current.width : 0),
                .height = geo.height > 0 ? geo.height : (s ? s->current.height : 0),
            };
        }
        if (m_xwayland_surface) {
            const auto* s = surface();
            return {
                .x = m_scene_tree->node.x,
                .y = m_scene_tree->node.y,
                .width = m_xwayland_surface->width > 0 ? static_cast<int>(m_xwayland_surface->width)
                                                       : (s ? s->current.width : 0),
                .height = m_xwayland_surface->height > 0 ? static_cast<int>(m_xwayland_surface->height)
                                                         : (s ? s->current.height : 0),
            };
        }
        return {};
    }

    void Window::center(const wlr_box& area) noexcept {
        const wlr_box box = geometry_box();
        if (box.width <= 0 || box.height <= 0) {
            return;
        }
        set_position(area.x + ((area.width - box.width) / 2), area.y + ((area.height - box.height) / 2));
    }

    void Window::start_grab(uint32_t edges) {
        m_compositor.dismiss_popups();
        m_placed = true;
        m_grab_box = geometry_box();
        m_resize_edges = edges;
        m_resizing = (edges != WLR_EDGE_NONE);
        if (m_resizing && m_xdg_toplevel) {
            wlr_xdg_toplevel_set_resizing(m_xdg_toplevel, true);
        }
    }

    void Window::update_grab(int dx, int dy) {
        if (!m_resizing) {
            set_position(m_grab_box.x + dx, m_grab_box.y + dy);
            return;
        }

        int width = m_grab_box.width;
        int height = m_grab_box.height;
        if (m_resize_edges & WLR_EDGE_LEFT) {
            width -= dx;
        } else if (m_resize_edges & WLR_EDGE_RIGHT) {
            width += dx;
        }
        if (m_resize_edges & WLR_EDGE_TOP) {
            height -= dy;
        } else if (m_resize_edges & WLR_EDGE_BOTTOM) {
            height += dy;
        }
        const int w = std::max(1, width);
        const int h = std::max(1, height);
        if (m_xdg_toplevel) {
            wlr_xdg_toplevel_set_size(m_xdg_toplevel, w, h);
        } else if (m_xwayland_surface) {
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
    }

    void Window::end_grab() noexcept {
        const bool was_resizing = m_resizing;
        if (m_resizing) {
            m_resizing = false;
            if (m_xdg_toplevel && m_xdg_toplevel->base->initialized) {
                wlr_xdg_toplevel_set_resizing(m_xdg_toplevel, false);
            } else {
                m_resize_edges = WLR_EDGE_NONE;
            }
        }
        if (!was_resizing && is_mapped()) {
            if (Output* out = m_compositor.focused_output(); out && output() != out) {
                set_output(out);
                if (m_workspace) {
                    m_workspace->arrange();
                }
                m_compositor.refocus();
            }
        }
    }

    Output* Window::output() const noexcept {
        return m_workspace ? &m_workspace->output() : nullptr;
    }

    void Window::set_output(Output* output) noexcept {
        if (this->output() == output) {
            return;
        }
        Workspace* old_ws = m_workspace;
        set_workspace(output ? &output->active_workspace() : nullptr);
        if (is_mapped()) {
            if (old_ws) {
                old_ws->arrange();
            }
            if (m_workspace) {
                m_workspace->arrange();
            }
        }
    }

    void Window::set_workspace(Workspace* ws) noexcept {
        if (ws) {
            m_workspace_id = ws->id();
        }
        if (m_workspace != ws) {
            if (m_workspace) {
                m_workspace->remove(this);
            }
            m_workspace = ws;
            reparent_to_workspace();
        }
        if (ws && is_mapped()) {
            ws->promote(this);
        }
    }

    void Window::reparent_to_workspace() noexcept {
        wlr_scene_tree* target = m_workspace ? m_workspace->tree() : m_compositor.scene().windows();
        wlr_scene_node_reparent(&m_scene_tree->node, target);
        if (surface()) {
            wlr_scene_subsurface_tree_set_clip(&m_scene_tree->node, nullptr);
        }
    }

    void Window::update_foreign_toplevel() noexcept {
        if (!m_ext_toplevel_handle) {
            return;
        }
        const wlr_ext_foreign_toplevel_handle_v1_state state{
            .title = m_title.c_str(),
            .app_id = m_app_id.c_str(),
        };
        wlr_ext_foreign_toplevel_handle_v1_update_state(m_ext_toplevel_handle, &state);
    }

    void Window::on_set_title() {
        if (m_xdg_toplevel) {
            m_title = m_xdg_toplevel->title ? m_xdg_toplevel->title : "";
        } else if (m_xwayland_surface) {
            m_title = m_xwayland_surface->title ? m_xwayland_surface->title : "";
        }
        wlr_log(WLR_DEBUG, "window title updated: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        update_foreign_toplevel();
    }

    void Window::on_set_app_id() {
        if (m_xdg_toplevel) {
            m_app_id = m_xdg_toplevel->app_id ? m_xdg_toplevel->app_id : "";
        } else if (m_xwayland_surface) {
            m_app_id = m_xwayland_surface->_class ? m_xwayland_surface->_class : "";
        }
        wlr_log(WLR_DEBUG, "window app_id updated: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        update_foreign_toplevel();
    }

    void Window::on_set_parent() {
        Window* parent = m_xdg_toplevel ? Window::try_from(m_xdg_toplevel->parent)
                                        : (m_xwayland_surface ? Window::try_from(m_xwayland_surface->parent) : nullptr);
        if (parent && parent->output()) {
            set_output(parent->output());
        }
        if (!m_floating && !m_floating_override && wants_floating()) {
            set_floating(true);
        }
    }

    void Window::on_set_hints() {
        if (m_xwayland_surface && !m_floating && !m_floating_override && wants_floating()) {
            set_floating(true);
        }
    }

    void Window::on_set_override_redirect() {
        if (m_xwayland_surface && m_xwayland_surface->override_redirect) {
            m_compositor.recreate_as_popup(this);
        }
    }

}
