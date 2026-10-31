#include "flux/window.hpp"
#include "flux/compositor.hpp"
#include "flux/output.hpp"
#include "flux/popup.hpp"
#include "flux/workspace.hpp"

namespace flux {

    Window::Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel)
        : m_compositor(compositor), m_xdg_toplevel(xdg_toplevel), m_workspace(&compositor.current_workspace()),
          m_output(compositor.focused_output()) {
        m_scene_tree = wlr_scene_xdg_surface_create(m_workspace->tree(), xdg_toplevel->base);
        m_scene_tree->node.data = this;
        xdg_toplevel->base->data = m_scene_tree;

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
    }

    void Window::on_map() {
        wlr_log(WLR_DEBUG, "window mapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());

        const wlr_ext_foreign_toplevel_handle_v1_state state{
            .title = m_title.c_str(),
            .app_id = m_app_id.c_str(),
        };
        m_ext_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(m_compositor.foreign_toplevel_list(), &state);

        m_compositor.map_window(this);
    }

    void Window::on_unmap() {
        if (m_ext_toplevel_handle) {
            wlr_ext_foreign_toplevel_handle_v1_destroy(m_ext_toplevel_handle);
            m_ext_toplevel_handle = nullptr;
        }

        wlr_log(WLR_DEBUG, "window unmapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        m_compositor.unmap_window(this);
    }

    void Window::on_commit() {
        if (m_xdg_toplevel->base->initial_commit) {
            wlr_xdg_toplevel_set_wm_capabilities(m_xdg_toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
            wlr_xdg_toplevel_set_tiled(m_xdg_toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
            if (!m_compositor.exclusive_layer()) {
                set_activated(true);
            }
            if (m_xdg_toplevel->requested.fullscreen) {
                set_fullscreen(true);
            } else {
                wlr_xdg_toplevel_set_size(m_xdg_toplevel, 0, 0);
            }
        }
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
        m_compositor.arrange_workspace(m_workspace);
        m_compositor.focus_window(m_compositor.focused_window());
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

        if (m_fullscreen) {
            if (m_xdg_toplevel->requested.fullscreen_output) {
                if (Output* out = m_compositor.output_from_wlr(m_xdg_toplevel->requested.fullscreen_output)) {
                    m_output = out;
                }
            }
            wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, true);
        } else {
            reparent_to_workspace();
            wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, false);
        }

        if (is_mapped()) {
            m_compositor.arrange_workspace(m_workspace);
            m_compositor.focus_window((m_fullscreen && m_workspace == &m_compositor.current_workspace())
                                          ? this
                                          : m_compositor.focused_window());
        } else if (m_fullscreen && m_output) {
            set_geometry(m_output->layout_box());
        }
    }

    void Window::set_geometry(const wlr_box& box) noexcept {
        wlr_scene_node_set_position(&m_scene_tree->node, box.x, box.y);
        for (const auto& popup : m_compositor.popups()) {
            if (popup->root_tree() == m_scene_tree) {
                popup->update_position();
            }
        }
        wlr_xdg_toplevel_set_size(m_xdg_toplevel, box.width, box.height);
    }

    void Window::reparent_to_workspace() noexcept {
        wlr_scene_node_reparent(&m_scene_tree->node, m_workspace->tree());
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
