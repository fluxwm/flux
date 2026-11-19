#include "flux/input/grab.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/output.hpp"
#include "flux/window.hpp"
#include "flux/workspace.hpp"

#include <algorithm>

namespace flux::input {

    bool Grab::is_valid() const {
        return !m_window->compositor().is_locked() && m_window->is_mapped() &&
               m_window == m_window->compositor().focused_window();
    }

    void MoveGrab::start() {
        m_window->prepare_floating_grab();
        m_initial_box = m_window->geometry_box();
    }

    void MoveGrab::update(int dx, int dy) {
        m_window->set_position(m_initial_box.x + dx, m_initial_box.y + dy);
    }

    void MoveGrab::end(bool commit) {
        if (commit && m_window->is_mapped()) {
            Compositor& comp = m_window->compositor();
            if (Output* out = comp.focused_output(); out && m_window->output() != out) {
                m_window->set_output(out);
                comp.refocus();
            }
        }
    }

    bool MoveGrab::is_valid() const {
        return Grab::is_valid() && m_window->is_floating();
    }

    void ResizeGrab::start() {
        m_window->prepare_floating_grab();
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
        m_window->prepare_tiled_grab();

        const std::vector<Window*> tiled = m_workspace.tiled_windows();
        for (Window* w : tiled) {
            w->set_tiled_resizing(true);
        }

        if (tiled.size() <= 2) {
            return;
        }

        const auto it = std::ranges::find(tiled, m_window);
        if (it == tiled.end() || it == tiled.begin()) {
            return;
        }

        const size_t idx = static_cast<size_t>(std::distance(tiled.begin(), it));
        if ((m_edges & WLR_EDGE_BOTTOM) && idx + 1 < tiled.size()) {
            m_top_win = tiled[idx];
            m_bot_win = tiled[idx + 1];
        } else if ((m_edges & WLR_EDGE_TOP) && idx > 1) {
            m_top_win = tiled[idx - 1];
            m_bot_win = tiled[idx];
        }

        if (m_top_win && m_bot_win) {
            m_top_weight = m_top_win->tile_weight();
            m_bot_weight = m_bot_win->tile_weight();
        }
    }

    void TiledResizeGrab::update(int dx, int dy) {
        const std::vector<Window*> tiled = m_workspace.tiled_windows();
        const size_t total = tiled.size();
        if (total < 2) {
            return;
        }

        const auto& usable = m_workspace.output().usable_area();
        const auto& gaps = m_workspace.output().compositor().config().layout().gaps;
        const int area_w = std::max(1, usable.width - (2 * gaps.outer));
        const int avail_w = std::max(1, area_w - gaps.inner);
        const int area_h = std::max(1, usable.height - (2 * gaps.outer));

        bool changed = false;
        if (m_edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
            const float delta_factor = static_cast<float>(dx) / static_cast<float>(avail_w);
            const float next_factor = std::clamp(m_initial_master_factor + delta_factor, 0.10f, 0.90f);
            if (next_factor != m_workspace.master_factor()) {
                m_workspace.set_master_factor(next_factor);
                changed = true;
            }
        }

        if ((m_edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && total > 2 && m_top_win && m_bot_win &&
            m_top_win != tiled[0] && m_bot_win != tiled[0] && m_top_win->is_tiled() && m_bot_win->is_tiled()) {
            const size_t stack_count = total - 1;
            const int avail_h = std::max(1, area_h - (static_cast<int>(stack_count - 1) * gaps.inner));

            float total_weight = 0.0f;
            for (size_t i = 1; i < tiled.size(); ++i) {
                total_weight += tiled[i]->tile_weight();
            }
            if (total_weight > 0.0f) {
                const float sum_pair_weight = m_top_weight + m_bot_weight;
                const float pair_h = static_cast<float>(avail_h) * (sum_pair_weight / total_weight);
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
        const auto tiled = m_workspace.tiled_windows();
        if (tiled.size() <= 1) {
            return false;
        }
        if (m_top_win && (m_top_win == tiled[0] || !std::ranges::contains(tiled, m_top_win))) {
            return false;
        }
        if (m_bot_win && (m_bot_win == tiled[0] || !std::ranges::contains(tiled, m_bot_win))) {
            return false;
        }
        return true;
    }

    const char* TiledResizeGrab::cursor_name() const noexcept {
        return wlr_xcursor_get_resize_name(static_cast<wlr_edges>(m_edges));
    }

    bool TiledResizeGrab::involves_window(const Window* window) const noexcept {
        return m_window == window || m_top_win == window || m_bot_win == window;
    }

}
