#include "flux/window.hpp"
#include "flux/compositor.hpp"

namespace flux {

    Window::Window(Compositor& compositor, wlr_xdg_toplevel* xdg_toplevel)
        : m_compositor(compositor), m_xdg_toplevel(xdg_toplevel) {
        m_scene_tree = wlr_scene_xdg_surface_create(&m_compositor.scene()->tree, xdg_toplevel->base);
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

        on_set_title();
        on_set_app_id();
    }

    void Window::on_map() {
        on_set_title();
        on_set_app_id();
        wlr_log(WLR_INFO, "Window mapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        m_compositor.map_window(this);
    }

    void Window::on_unmap() {
        wlr_log(WLR_INFO, "Window unmapped: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
        m_compositor.unmap_window(this);
    }

    void Window::on_commit() {
        if (m_xdg_toplevel->base->initial_commit) {
            set_activated(true);
            set_size(0, 0);
        }
        apply_pending_position();
    }

    void Window::on_destroy() {
        m_compositor.remove_window(this);
    }

    void Window::on_request_move() {
        m_compositor.begin_move(this);
    }

    void Window::on_request_resize(wlr_xdg_toplevel_resize_event* event) {
        m_compositor.begin_resize(this, event->edges);
    }

    void Window::on_request_maximize() {
        if (m_xdg_toplevel->base->initialized) {
            wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
        }
    }

    void Window::on_request_fullscreen() {
        if (m_xdg_toplevel->base->initialized) {
            wlr_xdg_surface_schedule_configure(m_xdg_toplevel->base);
        }
    }

    void Window::on_set_title() {
        m_title = m_xdg_toplevel->title ? m_xdg_toplevel->title : "";
        wlr_log(WLR_DEBUG, "Window title updated: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
    }

    void Window::on_set_app_id() {
        m_app_id = m_xdg_toplevel->app_id ? m_xdg_toplevel->app_id : "";
        wlr_log(WLR_DEBUG, "Window app_id updated: '%s' [%s]", m_title.c_str(), m_app_id.c_str());
    }

}
