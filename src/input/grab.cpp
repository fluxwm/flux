#include "frost/input/grab.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/output.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

#include <algorithm>

namespace frost::input {

    bool Grab::is_valid() const {
        return !m_window->compositor().is_locked() && m_window->is_mapped() &&
               m_window == m_window->compositor().focused_window();
    }

    void MoveGrab::start() {
        m_window->prepare_grab();
        m_initial_box = m_window->geometry_box();
    }

    void MoveGrab::update(int dx, int dy) {
        m_window->set_position(m_initial_box.x + dx, m_initial_box.y + dy);
    }

    void MoveGrab::end(bool commit) {
        if (commit && m_window->is_mapped()) {
            Compositor& comp = m_window->compositor();
            if (Output* out = comp.output_at(m_cursor_x, m_cursor_y); out && m_window->output() != out) {
                out->animation().cancel();
                m_window->set_output(out);
                comp.set_focused_output(out);
                comp.refocus();
            }
        }
    }

    bool MoveGrab::is_valid() const {
        return Grab::is_valid() && m_window->is_floating();
    }

    TiledMoveGrab::TiledMoveGrab(Window* window, uint32_t button, double cursor_x, double cursor_y) noexcept
        : Grab(window, button, cursor_x, cursor_y), m_workspace(*window->workspace()) {}

    void TiledMoveGrab::start() {
        m_window->prepare_grab();
        if (m_window->scene_tree()) {
            wlr_scene_node_raise_to_top(&m_window->scene_tree()->node);
        }
        m_initial_box = m_window->geometry_box();
    }

    void TiledMoveGrab::update(int dx, int dy) {
        m_window->move_scene_tree(m_initial_box.x + dx, m_initial_box.y + dy);
        m_workspace.swap_tiled_at(m_window, m_cursor_x, m_cursor_y);
    }

    void TiledMoveGrab::end(bool commit) {
        if (commit && m_window->is_mapped()) {
            Compositor& comp = m_window->compositor();
            if (Output* out = comp.output_at(m_cursor_x, m_cursor_y); out && m_window->output() != out) {
                out->animation().cancel();
                const wlr_box cur = m_window->geometry_box();
                m_window->set_position(cur.x, cur.y);
                m_window->set_output(out);
                out->active_workspace().swap_tiled_at(m_window, m_cursor_x, m_cursor_y);
                comp.set_focused_output(out);
                comp.refocus();
                return;
            }
        }
        m_workspace.restack();
        m_workspace.arrange();
    }

    bool TiledMoveGrab::is_valid() const {
        return Grab::is_valid() && m_window->is_tiled() && m_window->workspace() == &m_workspace;
    }

    void ResizeGrab::start() {
        m_window->prepare_grab();
        m_initial_box = m_window->geometry_box();
        m_window->begin_resize(m_edges, m_initial_box);
    }

    void ResizeGrab::update(int dx, int dy) {
        int width = m_initial_box.width;
        int height = m_initial_box.height;
        if (m_edges & WLR_EDGE_LEFT) {
            width -= dx;
        } else if (m_edges & WLR_EDGE_RIGHT) {
            width += dx;
        }
        if (m_edges & WLR_EDGE_TOP) {
            height -= dy;
        } else if (m_edges & WLR_EDGE_BOTTOM) {
            height += dy;
        }
        m_window->apply_resize(std::max(1, width), std::max(1, height));
    }

    void ResizeGrab::end(bool /*commit*/) {
        m_window->finish_resize();
    }

    bool ResizeGrab::is_valid() const {
        return Grab::is_valid() && m_window->is_floating();
    }

    const char* ResizeGrab::cursor_name() const noexcept {
        return wlr_xcursor_get_resize_name(static_cast<wlr_edges>(m_edges));
    }

    TiledResizeGrab::TiledResizeGrab(Window* window, uint32_t button, double cursor_x, double cursor_y,
                                     uint32_t edges) noexcept
        : Grab(window, button, cursor_x, cursor_y), m_workspace(*window->workspace()), m_edges(edges),
          m_initial_master_factor(m_workspace.master_factor()) {}

    void TiledResizeGrab::start() {
        m_workspace.snap_animations();
        m_window->prepare_grab();

        for (Window* w : m_workspace.windows()) {
            if (w->is_tiled()) {
                w->set_tiled_resizing(true);
            }
        }

        const TiledNeighbors n = m_workspace.tiled_neighbors(m_window);
        if (n.count <= 2 || !n.found || n.index == 0) {
            return;
        }

        if ((m_edges & WLR_EDGE_BOTTOM) && n.next) {
            m_top_win = m_window;
            m_bot_win = n.next;
        } else if ((m_edges & WLR_EDGE_TOP) && n.index > 1) {
            m_top_win = n.prev;
            m_bot_win = m_window;
        }

        if (m_top_win && m_bot_win) {
            m_top_weight = m_top_win->tile_weight();
            m_bot_weight = m_bot_win->tile_weight();
        }
    }

    void TiledResizeGrab::update(int dx, int dy) {
        const LayoutMetrics m = m_workspace.layout_metrics();
        if (m.tiled_count < 2) {
            return;
        }

        bool changed = false;
        if (m_edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
            const float delta_factor = static_cast<float>(dx) / static_cast<float>(m.avail_w);
            const float next_factor = std::clamp(m_initial_master_factor + delta_factor, 0.10f, 0.90f);
            if (next_factor != m_workspace.master_factor()) {
                m_workspace.set_master_factor(next_factor);
                changed = true;
            }
        }

        if ((m_edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && m.tiled_count > 2 && m_top_win && m_bot_win &&
            m_top_win != m.master && m_bot_win != m.master && m_top_win->is_tiled() && m_bot_win->is_tiled()) {
            if (m.total_stack_weight > 0.0f) {
                const float sum_pair_weight = m_top_weight + m_bot_weight;
                const float pair_h = static_cast<float>(m.avail_h) * (sum_pair_weight / m.total_stack_weight);
                if (pair_h > 80.0f) {
                    const float initial_top_h = pair_h * (m_top_weight / sum_pair_weight);
                    const float new_top_h = std::clamp(initial_top_h + static_cast<float>(dy), 40.0f, pair_h - 40.0f);
                    const float next_top = sum_pair_weight * (new_top_h / pair_h);
                    const float next_bot = sum_pair_weight * (1.0f - (new_top_h / pair_h));
                    if (next_top != m_top_win->tile_weight() || next_bot != m_bot_win->tile_weight()) {
                        m_top_win->set_tile_weight(next_top);
                        m_bot_win->set_tile_weight(next_bot);
                        changed = true;
                    }
                }
            }
        }

        if (changed) {
            m_workspace.arrange();
        }
    }

    void TiledResizeGrab::end(bool /*commit*/) {
        for (Window* w : m_workspace.windows()) {
            w->set_tiled_resizing(false);
        }
    }

    bool TiledResizeGrab::is_valid() const {
        if (!Grab::is_valid() || !m_window->is_tiled() || m_window->workspace() != &m_workspace ||
            m_window->compositor().config().layout().mode == Layout::Monocle) {
            return false;
        }
        const TiledNeighbors n = m_workspace.tiled_neighbors(m_window);
        if (n.count <= 1) {
            return false;
        }
        auto valid_stack_member = [&](const Window* w) {
            return !w || (w != n.master && std::ranges::contains(m_workspace.windows(), w) && w->is_tiled());
        };
        return valid_stack_member(m_top_win) && valid_stack_member(m_bot_win);
    }

    const char* TiledResizeGrab::cursor_name() const noexcept {
        return wlr_xcursor_get_resize_name(static_cast<wlr_edges>(m_edges));
    }

    bool TiledResizeGrab::involves_window(const Window* window) const noexcept {
        return m_window == window || m_top_win == window || m_bot_win == window;
    }

}
