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
    }

    Window::~Window() {
        if (m_ext_toplevel_handle) {
            wlr_ext_foreign_toplevel_handle_v1_destroy(m_ext_toplevel_handle);
        }
        if (m_scene_tree) {
            m_scene_tree->node.data = nullptr;
        }
        if (m_xdg_toplevel && m_xdg_toplevel->base) {
            m_xdg_toplevel->base->data = nullptr;
        }
    }

    bool Window::wants_floating() const noexcept {
        const auto& state = m_xdg_toplevel->current;
        return m_xdg_toplevel->parent != nullptr || (state.max_width > 0 && state.min_width == state.max_width) ||
               (state.max_height > 0 && state.min_height == state.max_height);
    }

    void Window::on_map() {
        wlr_log(WLR_DEBUG, "window mapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());

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
        m_activated = false;
        update_borders();

        wlr_log(WLR_DEBUG, "window unmapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        m_compositor.unmap_window(this);
    }

    void Window::on_commit() {
        if (m_xdg_toplevel->base->initial_commit) {
            if (auto* parent = Window::try_from(m_xdg_toplevel->parent); parent && parent->output()) {
                set_output(parent->output());
            }
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

        if (m_resize_edges != WLR_EDGE_NONE) {
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
            if (auto* parent = Window::try_from(m_xdg_toplevel->parent); parent && parent->output()) {
                set_output(parent->output());
            }
            set_floating(true);
        } else if (is_mapped() && is_floating() && !m_placed && m_workspace) {
            center(m_workspace->output().usable_area());
        }
        update_borders();
    }

    void Window::on_destroy() {
        m_compositor.remove_window(this);
    }

    void Window::on_request_maximize() {
        if (m_xdg_toplevel->base->initialized) {
            wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
        }
    }

    void Window::toggle_maximize() {
        if (m_fullscreen) {
            return;
        }

        m_maximized = !m_maximized;
        m_placed = false;
        if (!m_maximized && m_floating) {
            wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
        }
        if (m_workspace) {
            m_workspace->arrange();
        }
        m_compositor.refocus();
    }

    void Window::set_floating(bool floating) {
        m_floating = floating;
        m_placed = false;
        constexpr uint32_t edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
        wlr_xdg_toplevel_set_tiled(m_xdg_toplevel, m_floating ? 0 : edges);
        if (is_floating()) {
            wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
        }
        if (is_mapped()) {
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
        m_maximized = false;
        set_floating(floating);
    }

    void Window::on_request_fullscreen() {
        if (m_xdg_toplevel->base->initialized) {
            set_fullscreen(m_xdg_toplevel->requested.fullscreen);
        }
    }

    void Window::set_fullscreen(bool fullscreen) {
        if (m_fullscreen == fullscreen) {
            wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
            return;
        }

        m_fullscreen = fullscreen;
        m_placed = false;
        update_borders();

        if (m_fullscreen) {
            if (m_xdg_toplevel->requested.fullscreen_output) {
                if (Output* out = m_compositor.output_from_wlr(m_xdg_toplevel->requested.fullscreen_output)) {
                    set_output(out);
                }
            }
            wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, true);
        } else {
            reparent_to_workspace();
            wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, false);
            if (m_floating && !m_maximized) {
                wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
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
        m_decoration = decoration;
        m_decoration_listeners.clear();
        m_decoration_listeners.listen(&decoration->events.request_mode, this, &Window::on_request_decoration_mode);
        m_decoration_listeners.listen(&decoration->events.destroy, this, &Window::on_destroy_decoration);
        on_request_decoration_mode();
    }

    void Window::on_request_decoration_mode() {
        if (m_decoration && m_xdg_toplevel->base->initialized) {
            wlr_xdg_toplevel_decoration_v1_set_mode(m_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }

    void Window::on_destroy_decoration() {
        m_decoration_listeners.clear();
        m_decoration = nullptr;
    }

    void Window::set_position(int x, int y) noexcept {
        if (m_scene_tree->node.x == x && m_scene_tree->node.y == y) {
            return;
        }
        wlr_scene_node_set_position(&m_scene_tree->node, x, y);
        for (const auto& popup : m_compositor.popups()) {
            if (popup->root_tree() == m_scene_tree) {
                popup->update_position();
            }
        }
    }

    void Window::set_geometry(const wlr_box& box) noexcept {
        set_position(box.x, box.y);
        wlr_xdg_toplevel_set_size(m_xdg_toplevel, std::max(1, box.width), std::max(1, box.height));
        update_borders();
    }

    void Window::set_activated(bool activated) noexcept {
        if (m_activated == activated) {
            return;
        }
        m_activated = activated;
        wlr_xdg_toplevel_set_activated(m_xdg_toplevel, activated);
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

        wlr_scene_node_set_position(&m_borders[0]->node, -bw, -bw);
        wlr_scene_rect_set_size(m_borders[0], w + (2 * bw), bw);

        wlr_scene_node_set_position(&m_borders[1]->node, -bw, h);
        wlr_scene_rect_set_size(m_borders[1], w + (2 * bw), bw);

        wlr_scene_node_set_position(&m_borders[2]->node, -bw, 0);
        wlr_scene_rect_set_size(m_borders[2], bw, h);

        wlr_scene_node_set_position(&m_borders[3]->node, w, 0);
        wlr_scene_rect_set_size(m_borders[3], bw, h);

        for (auto* border : m_borders) {
            wlr_scene_rect_set_color(border, color.data());
            wlr_scene_node_set_enabled(&border->node, true);
        }
    }

    wlr_box Window::geometry_box() const noexcept {
        const wlr_box& geo = m_xdg_toplevel->base->geometry;
        return {
            .x = m_scene_tree->node.x,
            .y = m_scene_tree->node.y,
            .width = geo.width > 0 ? geo.width : surface()->current.width,
            .height = geo.height > 0 ? geo.height : surface()->current.height,
        };
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
        if (m_resizing) {
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
        wlr_xdg_toplevel_set_size(m_xdg_toplevel, std::max(1, width), std::max(1, height));
    }

    void Window::end_grab() noexcept {
        const bool was_resizing = m_resizing;
        if (m_resizing) {
            m_resizing = false;
            if (m_xdg_toplevel->base->initialized) {
                wlr_xdg_toplevel_set_resizing(m_xdg_toplevel, false);
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
        if (is_mapped() && old_ws) {
            old_ws->arrange();
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
        m_title = m_xdg_toplevel->title ? m_xdg_toplevel->title : "";
        wlr_log(WLR_DEBUG, "window title updated: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        update_foreign_toplevel();
    }

    void Window::on_set_app_id() {
        m_app_id = m_xdg_toplevel->app_id ? m_xdg_toplevel->app_id : "";
        wlr_log(WLR_DEBUG, "window app_id updated: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        update_foreign_toplevel();
    }

}
