#include "flux/window.hpp"
#include "flux/compositor.hpp"
#include "flux/output.hpp"
#include "flux/util.hpp"
#include "flux/workspace.hpp"

namespace flux {

    Window::Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel)
        : m_compositor(compositor), m_xdg_toplevel(xdg_toplevel), m_workspace(&compositor.current_workspace()) {
        m_scene_tree = wlr_scene_xdg_surface_create(m_workspace->tree(), xdg_toplevel->base);
        m_scene_tree->node.data = this;
        xdg_toplevel->base->data = m_scene_tree;

        m_listeners.listen(&xdg_toplevel->base->surface->events.map, this, &Window::on_map);
        m_listeners.listen(&xdg_toplevel->base->surface->events.unmap, this, &Window::on_unmap);
        m_listeners.listen(&xdg_toplevel->base->surface->events.commit, this, &Window::on_commit);
        m_listeners.listen(&xdg_toplevel->events.destroy, this, &Window::on_destroy);
        m_listeners.listen(&xdg_toplevel->events.request_move, this, &Window::on_request_move);
        m_listeners.listen(&xdg_toplevel->events.request_resize, this, &Window::on_request_resize);
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
            set_activated(true);
            if (m_xdg_toplevel->requested.fullscreen) {
                set_fullscreen(true);
            } else if (m_xdg_toplevel->requested.maximized) {
                set_maximized(true);
            } else {
                set_size(0, 0);
            }
        } else if (!m_maximized && !m_fullscreen && !m_xdg_toplevel->current.maximized &&
                   !m_xdg_toplevel->current.fullscreen &&
                   (m_saved_geometry.width <= 0 || m_saved_geometry.height <= 0) && width() > 0 && height() > 0) {
            m_compositor.center_window(this);
            save_geometry();
        }
        apply_pending_position();
    }

    void Window::on_destroy() {
        m_compositor.remove_window(this);
    }

    void Window::on_request_move() {
        if (!m_maximized && !m_fullscreen) {
            m_compositor.begin_move(this);
        }
    }

    void Window::on_request_resize(wlr_xdg_toplevel_resize_event* event) {
        if (!m_maximized && !m_fullscreen) {
            m_compositor.begin_resize(this, event->edges);
        }
    }

    void Window::on_request_maximize() {
        if (m_xdg_toplevel->base->initialized) {
            set_maximized(m_xdg_toplevel->requested.maximized);
        }
    }

    void Window::set_maximized(bool maximize) {
        if (m_fullscreen || m_maximized == maximize) {
            wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
            return;
        }

        m_maximized = maximize;

        if (m_maximized) {
            save_geometry();

            if (Output* out = m_compositor.window_output(this)) {
                set_geometry(out->usable_area());
            }
            wlr_xdg_toplevel_set_maximized(m_xdg_toplevel, true);
        } else {
            wlr_xdg_toplevel_set_maximized(m_xdg_toplevel, false);
            restore_saved_geometry();
        }
    }

    void Window::on_request_fullscreen() {
        if (m_xdg_toplevel->base->initialized) {
            const bool wants_fullscreen = m_xdg_toplevel->requested.fullscreen;
            set_fullscreen(wants_fullscreen);
            if (wants_fullscreen && is_mapped() && m_workspace == &m_compositor.current_workspace() &&
                m_compositor.focused_window() != this) {
                m_compositor.focus_window(this);
            }
        }
    }

    void Window::set_fullscreen(bool fullscreen) {
        if (m_fullscreen == fullscreen) {
            wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
            return;
        }

        m_fullscreen = fullscreen;

        if (m_fullscreen) {
            if (!m_maximized) {
                save_geometry();
            }

            Output* out = nullptr;
            if (m_xdg_toplevel->requested.fullscreen_output) {
                out = m_compositor.output_from_wlr(m_xdg_toplevel->requested.fullscreen_output);
            }
            if (!out) {
                out = m_compositor.window_output(this);
            }

            if (out) {
                set_geometry(out->layout_box());
            }

            if (m_compositor.focused_window() == this) {
                wlr_scene_node_reparent(&m_scene_tree->node, m_compositor.scene().fullscreen());
            }
            wlr_xdg_toplevel_set_maximized(m_xdg_toplevel, false);
            wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, true);
        } else {
            reparent_to_workspace();
            wlr_xdg_toplevel_set_fullscreen(m_xdg_toplevel, false);

            if (m_maximized) {
                if (Output* out = m_compositor.window_output(this)) {
                    set_geometry(out->usable_area());
                }
                wlr_xdg_toplevel_set_maximized(m_xdg_toplevel, true);
            } else {
                restore_saved_geometry();
            }

            raise_to_top();
        }
    }

    void Window::reparent_to_workspace() noexcept {
        if (m_workspace) {
            wlr_scene_node_reparent(&m_scene_tree->node, m_workspace->tree());
        }
    }

    void Window::save_geometry() noexcept {
        if (width() > 0 && height() > 0) {
            m_saved_geometry = box();
        }
    }

    void Window::restore_saved_geometry() {
        if (m_saved_geometry.width <= 0 || m_saved_geometry.height <= 0) {
            set_size(0, 0);
            return;
        }

        const double cx = m_saved_geometry.x + (m_saved_geometry.width / 2.0);
        const double cy = m_saved_geometry.y + (m_saved_geometry.height / 2.0);
        if (!wlr_output_layout_output_at(m_compositor.output_layout(), cx, cy)) {
            if (Output* out = m_compositor.focused_output()) {
                m_saved_geometry = center_box(out->usable_area(), m_saved_geometry.width, m_saved_geometry.height);
            }
        }

        set_geometry(m_saved_geometry);
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
