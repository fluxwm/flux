#include "frost/window/xdg.hpp"
#include "frost/compositor.hpp"
#include "frost/workspace.hpp"

#include <algorithm>

namespace frost::window {

    namespace {

        [[nodiscard]] constexpr bool serial_ge(uint32_t a, uint32_t b) noexcept {
            return static_cast<int32_t>(a - b) >= 0;
        }

    }

    Xdg::Xdg(Compositor& compositor, wlr_xdg_toplevel* toplevel)
        : Window(compositor, toplevel->title, toplevel->app_id), m_toplevel(toplevel) {
        attach_scene_tree(wlr_scene_xdg_surface_create(initial_parent_tree(), toplevel->base));
        toplevel->base->data = m_scene_tree;

        m_listeners.listen(&toplevel->base->surface->events.map, this, &Xdg::on_map);
        m_listeners.listen(&toplevel->base->surface->events.unmap, this, &Xdg::on_unmap);
        m_listeners.listen(&toplevel->base->surface->events.commit, this, &Xdg::on_commit);
        m_listeners.listen(&toplevel->events.destroy, this, &Xdg::on_destroy);
        m_listeners.listen(&toplevel->events.request_maximize, this, &Xdg::on_request_maximize);
        m_listeners.listen(&toplevel->events.request_fullscreen, this, &Xdg::on_request_fullscreen);
        m_listeners.listen(&toplevel->events.set_title, this, &Xdg::on_set_title);
        m_listeners.listen(&toplevel->events.set_app_id, this, &Xdg::on_set_app_id);
        m_listeners.listen(&toplevel->events.set_parent, this, &Xdg::on_set_parent);
    }

    Xdg::~Xdg() {
        m_decoration_listeners.clear();
        m_listeners.clear();
        if (m_toplevel && m_toplevel->base) {
            m_toplevel->base->data = nullptr;
        }
    }

    wlr_surface* Xdg::surface() const noexcept {
        return m_toplevel->base->surface;
    }

    std::pair<int, int> Xdg::preferred_size() const noexcept {
        const wlr_box& geo = m_toplevel->base->geometry;
        return {geo.width, geo.height};
    }

    void Xdg::set_tiled_resizing(bool resizing) noexcept {
        if (m_toplevel->base->initialized && m_toplevel->scheduled.resizing != resizing) {
            wlr_xdg_toplevel_set_resizing(m_toplevel, resizing);
        }
    }

    void Xdg::set_decoration(wlr_xdg_toplevel_decoration_v1* decoration) {
        m_decoration = decoration;
        m_decoration_listeners.clear();
        m_decoration_listeners.listen(&decoration->events.request_mode, this, &Xdg::on_request_decoration_mode);
        m_decoration_listeners.listen(&decoration->events.destroy, this, &Xdg::on_destroy_decoration);
        on_request_decoration_mode();
    }

    void Xdg::close() noexcept {
        wlr_xdg_toplevel_send_close(m_toplevel);
    }

    void Xdg::apply_activated(bool activated) noexcept {
        wlr_xdg_toplevel_set_activated(m_toplevel, activated);
    }

    void Xdg::apply_fullscreen(bool fullscreen) noexcept {
        if (fullscreen) {
            wlr_xdg_toplevel_set_tiled(m_toplevel, 0);
            wlr_xdg_toplevel_set_fullscreen(m_toplevel, true);
        } else {
            wlr_xdg_toplevel_set_fullscreen(m_toplevel, false);
            if (!m_floating) {
                wlr_xdg_toplevel_set_tiled(m_toplevel, tiled_edges);
            }
        }
    }

    void Xdg::apply_tiled(uint32_t edges) noexcept {
        if (m_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_tiled(m_toplevel, edges);
        }
    }

    void Xdg::schedule_configure() noexcept {
        if (m_toplevel->base->initialized) {
            wlr_xdg_surface_schedule_configure(m_toplevel->base);
        }
    }

    bool Xdg::configure_geometry(const wlr_box& box) noexcept {
        if (!m_resizing) {
            m_resize_serial = 0;
            m_pending_center_serial = 0;
        }
        const int w = std::max(1, box.width);
        const int h = std::max(1, box.height);
        const wlr_box cur = geometry_box();
        const uint32_t serial = wlr_xdg_toplevel_set_size(m_toplevel, w, h);
        const bool shrinking =
            (w < cur.width && w != m_toplevel->current.width && w >= m_toplevel->current.min_width) ||
            (h < cur.height && h != m_toplevel->current.height && h >= m_toplevel->current.min_height);
        if (!m_animation.is_moving() && m_animation.can_animate() &&
            (m_animation.base_x() != box.x || m_animation.base_y() != box.y) && shrinking) {
            if (m_pending_move_serial == 0) {
                m_pending_move_serial = serial;
            }
            return true;
        }
        return false;
    }

    void Xdg::apply_resize(int w, int h) noexcept {
        const auto& s = m_toplevel->current;
        clamp_size(w, h, s.min_width, s.min_height, s.max_width, s.max_height);
        const int cur_w = m_toplevel->scheduled.width > 0 ? m_toplevel->scheduled.width : m_grab_box.width;
        const int cur_h = m_toplevel->scheduled.height > 0 ? m_toplevel->scheduled.height : m_grab_box.height;
        if (w != cur_w || h != cur_h) {
            const uint32_t serial = wlr_xdg_toplevel_set_size(m_toplevel, w, h);
            if (m_resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP)) {
                m_resize_serial = serial;
            }
        }
    }

    void Xdg::start_resizing() noexcept {
        m_resize_serial = 0;
        if (m_resizing) {
            wlr_xdg_toplevel_set_resizing(m_toplevel, true);
        }
    }

    void Xdg::end_resizing() noexcept {
        bool clear_resize = true;
        if (m_toplevel->base->initialized) {
            wlr_xdg_toplevel_set_resizing(m_toplevel, false);
            clear_resize =
                (m_resize_serial == 0 || serial_ge(m_toplevel->base->current.configure_serial, m_resize_serial));
        }
        if (clear_resize) {
            m_resize_edges = WLR_EDGE_NONE;
            m_resize_serial = 0;
        }
    }

    void Xdg::request_default_floating_size() noexcept {
        if (m_toplevel->base->initialized) {
            m_pending_center_serial = wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
        }
    }

    Window* Xdg::query_parent() const noexcept {
        return Window::try_from(m_toplevel->parent);
    }

    bool Xdg::wants_floating() const noexcept {
        const auto& state = m_toplevel->current;
        return m_toplevel->parent != nullptr || (state.max_width > 0 && state.min_width == state.max_width) ||
               (state.max_height > 0 && state.min_height == state.max_height);
    }

    void Xdg::on_commit() {
        if (m_toplevel->base->initial_commit) {
            on_request_decoration_mode();
            wlr_xdg_toplevel_set_wm_capabilities(m_toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
            m_floating = wants_floating();
            wlr_xdg_toplevel_set_tiled(m_toplevel,
                                       (!m_floating && !m_toplevel->requested.fullscreen) ? tiled_edges : 0);
            if (m_toplevel->requested.fullscreen) {
                set_fullscreen(true, m_compositor.output_from_wlr(m_toplevel->requested.fullscreen_output));
            } else if (!m_floating && m_workspace) {
                set_geometry(m_workspace->next_tile_box());
            } else {
                wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
            }
            return;
        }

        const int old_x = m_animation.base_x();
        const int old_y = m_animation.base_y();

        if (m_resize_edges != WLR_EDGE_NONE) {
            if (is_floating() && (m_resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP))) {
                const wlr_box box = geometry_box();
                const int x =
                    (m_resize_edges & WLR_EDGE_LEFT) ? (m_grab_box.x + m_grab_box.width - box.width) : m_grab_box.x;
                const int y =
                    (m_resize_edges & WLR_EDGE_TOP) ? (m_grab_box.y + m_grab_box.height - box.height) : m_grab_box.y;
                set_position(x, y);
            }
            if (!m_resizing &&
                (m_resize_serial == 0 || serial_ge(m_toplevel->base->current.configure_serial, m_resize_serial))) {
                m_resize_edges = WLR_EDGE_NONE;
                m_resize_serial = 0;
            }
        }

        if (m_pending_center_serial != 0 &&
            serial_ge(m_toplevel->base->current.configure_serial, m_pending_center_serial)) {
            m_pending_center_serial = 0;
            update_borders();
        }

        if (m_pending_move_serial != 0 &&
            serial_ge(m_toplevel->base->current.configure_serial, m_pending_move_serial)) {
            m_pending_move_serial = 0;
            m_animation.apply_position();
            update_borders();
        }

        if (!m_floating && !m_floating_override && wants_floating()) {
            on_set_parent();
        } else {
            check_center();
        }

        finish_commit(old_x, old_y);
    }

    void Xdg::on_request_maximize() {
        if (m_toplevel->base->initialized) {
            wlr_xdg_surface_schedule_configure(m_toplevel->base);
        }
    }

    void Xdg::on_request_fullscreen() {
        if (m_toplevel->base->initialized) {
            Output* target = m_toplevel->requested.fullscreen
                                 ? m_compositor.output_from_wlr(m_toplevel->requested.fullscreen_output)
                                 : nullptr;
            set_fullscreen(m_toplevel->requested.fullscreen, target);
        }
    }

    void Xdg::on_set_title() {
        update_title(m_toplevel->title);
    }

    void Xdg::on_set_app_id() {
        update_app_id(m_toplevel->app_id);
    }

    void Xdg::on_request_decoration_mode() {
        if (m_decoration && m_toplevel->base->initialized) {
            wlr_xdg_toplevel_decoration_v1_set_mode(m_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }

    void Xdg::on_destroy_decoration() {
        m_decoration_listeners.clear();
        m_decoration = nullptr;
    }

}
