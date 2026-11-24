#include "frost/window.hpp"
#include "frost/compositor.hpp"
#include "frost/input.hpp"
#include "frost/log.hpp"
#include "frost/output.hpp"
#include "frost/workspace.hpp"

#include <algorithm>

namespace frost {

    namespace {

        [[nodiscard]] Workspace* initial_workspace(Compositor& compositor) noexcept {
            Output* out = compositor.focused_output();
            return out ? &out->active_workspace() : nullptr;
        }

    }

    Window::Window(Compositor& compositor, const char* title, const char* app_id)
        : m_compositor(compositor), m_animation(*this), m_workspace(initial_workspace(compositor)),
          m_workspace_id(m_workspace ? m_workspace->id() : 1), m_title(title ? title : ""),
          m_app_id(app_id ? app_id : "") {}

    Window::~Window() {
        if (m_ext_toplevel_handle) {
            wlr_ext_foreign_toplevel_handle_v1_destroy(m_ext_toplevel_handle);
            m_ext_toplevel_handle = nullptr;
        }
        if (m_scene_tree) {
            m_scene_tree->node.data = nullptr;
        }
    }

    wlr_scene_tree* Window::initial_parent_tree() const noexcept {
        return m_workspace ? m_workspace->tree() : m_compositor.scene().windows();
    }

    void Window::attach_scene_tree(wlr_scene_tree* tree) noexcept {
        m_scene_tree = tree;
        m_scene_tree->node.data = this;
        m_border.init(m_scene_tree);
    }

    void Window::clamp_size(int& w, int& h, int min_w, int min_h, int max_w, int max_h) noexcept {
        w = std::max(w, min_w);
        h = std::max(h, min_h);
        if (max_w >= std::max(1, min_w)) {
            w = std::min(w, max_w);
        }
        if (max_h >= std::max(1, min_h)) {
            h = std::min(h, max_h);
        }
    }

    void Window::on_map() {
        log_debug("window mapped: '{}' [{}]", m_title, m_app_id);

        const auto state = foreign_toplevel_state();
        m_ext_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(m_compositor.foreign_toplevel_list(), &state);

        m_compositor.map_window(this);
        if (m_placed) {
            m_animation.set_has_initial_position(true);
        }
        m_committed_width = surface()->current.width;
        m_committed_height = surface()->current.height;
        if (m_animation.can_animate() && !m_fullscreen) {
            m_animation.start_open();
        } else {
            update_borders();
        }
    }

    void Window::on_unmap() {
        const double open_progress = m_animation.open_progress();
        if (m_animation.is_opening()) {
            m_animation.reset_open_transform();
        }

        if (m_workspace) {
            m_workspace->add_closing_window(this, open_progress);
        }

        snap_animation();
        m_animation.set_has_initial_position(false);
        if (m_ext_toplevel_handle) {
            wlr_ext_foreign_toplevel_handle_v1_destroy(m_ext_toplevel_handle);
            m_ext_toplevel_handle = nullptr;
        }

        if (m_fullscreen) {
            m_fullscreen = false;
            wlr_scene_node_set_enabled(&m_scene_tree->node, true);
            reparent_to_workspace();
        }
        m_maximized = false;
        m_floating_override = false;
        m_tile_weight = 1.0f;
        reset_placement_state();
        m_floating_box = {};
        m_committed_width = 0;
        m_committed_height = 0;
        m_border_width = 0;
        m_border_height = 0;
        m_activated = false;
        m_border.hide();

        log_debug("window unmapped: '{}' [{}]", m_title, m_app_id);
        m_compositor.unmap_window(this);
    }

    void Window::finish_commit(int old_x, int old_y) {
        if (is_mapped()) {
            const auto* s = surface();
            const bool size_changed =
                (s->current.width != m_committed_width || s->current.height != m_committed_height);
            const bool pos_changed = (m_animation.base_x() != old_x || m_animation.base_y() != old_y);
            m_committed_width = s->current.width;
            m_committed_height = s->current.height;
            if (m_animation.is_opening()) {
                m_animation.apply_open_transform();
            } else if (const wlr_box box = geometry_box();
                       box.width != m_border_width || box.height != m_border_height) {
                update_borders();
            }
            if ((size_changed || pos_changed) && !is_resizing()) {
                m_compositor.update_cursor_focus();
            }
        }
    }

    void Window::check_center() noexcept {
        if (is_mapped() && is_floating() && !m_placed && m_workspace && !is_pending_center()) {
            center(m_workspace->output().usable_area());
        }
    }

    void Window::on_destroy() {
        m_compositor.remove_window(this);
    }

    void Window::toggle_maximize() {
        if (m_fullscreen) {
            return;
        }
        m_compositor.input().cursor().cancel_grab(this);

        save_floating_box();

        m_maximized = !m_maximized;
        reset_placement_state();
        apply_maximized(m_maximized);
        if (is_mapped()) {
            m_compositor.dismiss_popups(m_scene_tree);
            restore_floating_box();
            if (m_workspace) {
                m_workspace->arrange();
            }
            m_compositor.refocus();
        }
    }

    void Window::save_floating_box() noexcept {
        if (is_mapped() && is_floating() && m_placed && !is_pending_center() && !is_pending_move()) {
            m_floating_box = geometry_box();
            m_floating_box.x = m_animation.target_x();
            m_floating_box.y = m_animation.target_y();
        }
    }

    void Window::reset_placement_state() noexcept {
        m_placed = false;
        m_resize_edges = WLR_EDGE_NONE;
        clear_pending_serials();
    }

    void Window::restore_floating_box() noexcept {
        if (!is_mapped() || !is_floating()) {
            return;
        }
        if (m_floating_box.width > 0 && m_floating_box.height > 0) {
            m_placed = true;
            if (m_workspace) {
                const wlr_box& usable = m_workspace->output().usable_area();
                wlr_box intersection{};
                if (usable.width > 0 && usable.height > 0 &&
                    !wlr_box_intersection(&intersection, &m_floating_box, &usable)) {
                    m_floating_box.width = std::min(m_floating_box.width, usable.width);
                    m_floating_box.height = std::min(m_floating_box.height, usable.height);
                    m_floating_box.x = usable.x + ((usable.width - m_floating_box.width) / 2);
                    m_floating_box.y = usable.y + ((usable.height - m_floating_box.height) / 2);
                }
            }
            set_geometry(m_floating_box);
            return;
        }

        request_default_floating_size();
    }

    void Window::set_floating(bool floating) {
        m_compositor.input().cursor().cancel_grab(this);
        save_floating_box();
        m_floating = floating;
        reset_placement_state();
        apply_tiled(m_floating ? 0 : tiled_edges);
        if (is_mapped()) {
            m_compositor.dismiss_popups(m_scene_tree);
            restore_floating_box();
            if (m_workspace) {
                m_workspace->arrange();
            }
            m_compositor.refocus();
        }
        update_borders();
    }

    void Window::set_floating_in_grab(bool floating, const wlr_box& box) noexcept {
        m_floating_override = true;
        m_floating_box = box;
        m_floating = floating;
        if (floating) {
            m_placed = true;
            apply_tiled(0);
            set_position(box.x, box.y);
        } else {
            reset_placement_state();
            apply_tiled(tiled_edges);
        }
        update_borders();
    }

    void Window::toggle_floating() {
        if (m_fullscreen) {
            return;
        }

        const bool floating = !is_floating();
        m_floating_override = true;
        if (floating && m_floating_box.width <= 0 && m_floating_box.height <= 0 && is_mapped() && m_workspace) {
            const wlr_box& usable = m_workspace->output().usable_area();
            const wlr_box cur = geometry_box();
            const bool keep_size = !m_maximized && !m_workspace->is_solitary() && cur.width > 0 && cur.height > 0;
            const int w = keep_size ? std::min(cur.width, usable.width) : std::max(1, (usable.width * 3) / 4);
            const int h = keep_size ? std::min(cur.height, usable.height) : std::max(1, (usable.height * 3) / 4);
            m_floating_box = {
                .x = usable.x + ((usable.width - w) / 2),
                .y = usable.y + ((usable.height - h) / 2),
                .width = w,
                .height = h,
            };
        }
        if (m_maximized) {
            m_maximized = false;
            apply_maximized(false);
        }
        set_floating(floating);
    }

    void Window::set_fullscreen(bool fullscreen) {
        if (m_fullscreen == fullscreen) {
            schedule_configure();
            return;
        }
        m_compositor.input().cursor().cancel_grab(this);

        save_floating_box();

        m_fullscreen = fullscreen;
        reset_placement_state();

        if (m_fullscreen) {
            if (m_animation.is_opening()) {
                m_animation.reset_open_transform();
            }
            apply_fullscreen(true);
        } else {
            wlr_scene_node_set_enabled(&m_scene_tree->node, true);
            reparent_to_workspace();
            apply_fullscreen(false);
        }

        if (is_mapped()) {
            m_compositor.dismiss_popups(m_scene_tree);
            restore_floating_box();
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
        update_borders();
    }

    void Window::snap_animation() noexcept {
        const bool was_opening = m_animation.is_opening();
        const bool had_pending_move = is_pending_move();
        clear_pending_move();
        m_animation.snap();
        if (had_pending_move && !was_opening) {
            update_borders();
        }
    }

    void Window::set_position(int x, int y) noexcept {
        if (!m_animation.is_active() && m_animation.target_x() == x && m_animation.target_y() == y &&
            m_animation.base_x() == x && m_animation.base_y() == y) {
            if (is_mapped()) {
                m_animation.set_has_initial_position(true);
            }
            return;
        }

        m_animation.set_target(x, y);

        const bool had_pending_move = is_pending_move();
        clear_pending_move();

        configure_position(x, y);

        m_animation.apply_position();
        if (had_pending_move) {
            update_borders();
        }
    }

    void Window::set_geometry(const wlr_box& box) noexcept {
        if (is_grabbed() && !is_resizing()) {
            return;
        }
        if (!m_resizing) {
            m_resize_edges = WLR_EDGE_NONE;
        }
        m_animation.set_target(box.x, box.y);
        if (configure_geometry(box)) {
            return;
        }
        clear_pending_move();
        m_animation.apply_position();
        update_borders();
    }

    void Window::set_activated(bool activated) noexcept {
        if (m_activated == activated) {
            return;
        }
        m_activated = activated;
        apply_activated(activated);
        update_borders();
    }

    bool Window::has_visible_borders() const noexcept {
        if (m_fullscreen || is_pending_center() || (is_pending_move() && !m_border.is_enabled())) {
            return false;
        }
        const auto& layout_cfg = m_compositor.config().layout();
        const auto& border_cfg = layout_cfg.border;
        if (border_cfg.width <= 0) {
            return false;
        }
        const auto& color = m_activated ? border_cfg.active_color : border_cfg.inactive_color;
        if (color[3] <= 0.0f) {
            return false;
        }
        if (layout_cfg.gaps.smart && (m_maximized || (is_tiled() && (!m_workspace || m_workspace->is_solitary())))) {
            return false;
        }
        return true;
    }

    void Window::update_borders() noexcept {
        if (!is_mapped()) {
            m_border.hide();
            return;
        }
        if (!m_fullscreen && (is_pending_center() || (is_pending_move() && !m_border.is_enabled()))) {
            return;
        }

        const wlr_box box = geometry_box();
        m_border_width = box.width;
        m_border_height = box.height;

        if (!has_visible_borders()) {
            m_border.hide();
            return;
        }

        const double p = m_animation.open_progress();
        const double scale = open_close_scale(p);
        const auto& border_cfg = m_compositor.config().layout().border;
        const auto& color = m_activated ? border_cfg.active_color : border_cfg.inactive_color;

        m_border.update(scaled_extent(m_border_width, scale), scaled_extent(m_border_height, scale), border_cfg.width,
                        color, static_cast<float>(p));
    }

    wlr_box Window::geometry_box() const noexcept {
        const auto [pref_w, pref_h] = preferred_size();
        const auto* s = surface();
        const int surf_w = (s && s->current.width > 0) ? s->current.width : m_committed_width;
        const int surf_h = (s && s->current.height > 0) ? s->current.height : m_committed_height;
        return {
            .x = m_animation.base_x(),
            .y = m_animation.base_y(),
            .width = pref_w > 0 ? pref_w : surf_w,
            .height = pref_h > 0 ? pref_h : surf_h,
        };
    }

    void Window::center(const wlr_box& area) noexcept {
        const wlr_box box = geometry_box();
        if (box.width <= 0 || box.height <= 0) {
            return;
        }
        m_placed = true;
        set_position(area.x + ((area.width - box.width) / 2), area.y + ((area.height - box.height) / 2));
    }

    bool Window::is_grabbed() const noexcept {
        return m_compositor.input().cursor().grabbed_window() == this;
    }

    bool Window::is_resizing() const noexcept {
        return m_resizing || (m_workspace && m_workspace->is_resizing());
    }

    void Window::prepare_grab() noexcept {
        m_compositor.dismiss_popups();
        clear_pending_center();
        if (is_floating()) {
            m_placed = true;
        }
        if (m_animation.is_moving() || is_pending_move()) {
            set_position(m_animation.base_x(), m_animation.base_y());
        } else if (m_animation.is_opening()) {
            m_animation.reset_open_transform();
        }
        m_resize_edges = WLR_EDGE_NONE;
        clear_pending_serials();
    }

    void Window::begin_resize(uint32_t edges, const wlr_box& grab_box) noexcept {
        m_grab_box = grab_box;
        m_resize_edges = edges;
        m_resizing = (edges != WLR_EDGE_NONE);
        start_resizing();
    }

    void Window::finish_resize() noexcept {
        if (m_resizing) {
            m_resizing = false;
            end_resizing();
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
            m_compositor.input().cursor().cancel_grab(this);
            const double open_progress = m_animation.open_progress();
            if (m_animation.is_opening()) {
                m_animation.reset_open_transform();
            }
            if (m_workspace) {
                if (m_animation.has_initial_position() && ws && &m_workspace->output() == &ws->output()) {
                    m_workspace->add_closing_window(this, open_progress);
                }
                m_workspace->remove(this);
            }
            snap_animation();
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

    wlr_ext_foreign_toplevel_handle_v1_state Window::foreign_toplevel_state() const noexcept {
        return {
            .title = m_title.c_str(),
            .app_id = m_app_id.c_str(),
        };
    }

    void Window::update_foreign_toplevel() noexcept {
        if (!m_ext_toplevel_handle) {
            return;
        }
        const auto state = foreign_toplevel_state();
        wlr_ext_foreign_toplevel_handle_v1_update_state(m_ext_toplevel_handle, &state);
    }

    void Window::update_title(const char* title) {
        m_title = title ? title : "";
        log_debug("window title updated: '{}' [{}]", m_title, m_app_id);
        update_foreign_toplevel();
    }

    void Window::update_app_id(const char* app_id) {
        m_app_id = app_id ? app_id : "";
        log_debug("window app_id updated: '{}' [{}]", m_title, m_app_id);
        update_foreign_toplevel();
    }

    void Window::on_set_parent() {
        Window* parent = query_parent();
        const bool output_changed =
            parent && parent->output() && parent->output()->is_enabled() && parent->output() != output();
        if (output_changed) {
            set_placed(false);
            set_output(parent->output());
        }
        if (!m_floating && !m_floating_override && wants_floating()) {
            set_floating(true);
        } else if (output_changed && is_mapped()) {
            m_compositor.refocus();
        }
    }

}
