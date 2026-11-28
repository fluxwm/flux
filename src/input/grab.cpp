#include "frost/input/grab.hpp"
#include "frost/compositor.hpp"
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
                                     std::unique_ptr<layout::ResizeSession> session) noexcept
        : Grab(window, button, cursor_x, cursor_y), m_workspace(*window->workspace()), m_session(std::move(session)) {}

    void TiledResizeGrab::start() {
        m_workspace.snap_animations();
        m_window->prepare_grab();

        for (Window* w : m_workspace.windows()) {
            if (w->is_tiled()) {
                w->set_tiled_resizing(true);
            }
        }
    }

    void TiledResizeGrab::update(int dx, int dy) {
        if (m_session && m_session->update(m_workspace.layout_context(), dx, dy)) {
            m_workspace.arrange();
        }
    }

    void TiledResizeGrab::end(bool /*commit*/) {
        m_session.reset();
        for (Window* w : m_workspace.windows()) {
            w->set_tiled_resizing(false);
        }
    }

    bool TiledResizeGrab::is_valid() const {
        return Grab::is_valid() && m_window->is_tiled() && m_window->workspace() == &m_workspace && m_session &&
               m_session->is_valid(m_workspace.layout_context());
    }

    const char* TiledResizeGrab::cursor_name() const noexcept {
        if (!m_session) {
            return wlr_xcursor_get_resize_name(WLR_EDGE_NONE);
        }
        return wlr_xcursor_get_resize_name(static_cast<wlr_edges>(m_session->edges()));
    }

    bool TiledResizeGrab::involves_window(const Window* window) const noexcept {
        return m_window == window || (m_session && m_session->involves_window(window));
    }

}
