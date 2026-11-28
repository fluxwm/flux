#include "frost/workspace.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/input.hpp"
#include "frost/output.hpp"
#include "frost/scene.hpp"
#include "frost/window.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>

namespace frost {

    Workspace::Workspace(uint32_t id, Output& output, Scene& scene, wlr_ext_workspace_group_handle_v1* group)
        : m_id(id), m_output(output), m_tree(wlr_scene_tree_create(scene.windows())),
          m_layout(layout::create(output.compositor().config().layout().mode)) {
        wlr_scene_node_set_enabled(&m_tree->node, false);

        const std::string name = std::to_string(id);
        const char* out_name = output.raw() ? output.raw()->name : nullptr;
        const std::string handle_id = (out_name && *out_name != '\0') ? std::format("{}:{}", out_name, id) : "";
        constexpr uint32_t caps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
        m_handle =
            wlr_ext_workspace_handle_v1_create(group->manager, handle_id.empty() ? nullptr : handle_id.c_str(), caps);
        m_handle->data = this;
        wlr_ext_workspace_handle_v1_set_group(m_handle, group);
        wlr_ext_workspace_handle_v1_set_name(m_handle, name.c_str());
        wlr_ext_workspace_handle_v1_set_coordinates(m_handle, &m_id, 1);
    }

    Workspace::~Workspace() {
        snap_closing_animations();
        wlr_ext_workspace_handle_v1_destroy(m_handle);
        if (m_tree) {
            wlr_scene_node_destroy(&m_tree->node);
        }
    }

    void Workspace::activate() noexcept {
        wlr_ext_workspace_handle_v1_set_active(m_handle, true);
        if (m_output.animation().is_busy()) {
            return;
        }
        clear_slide_clips();
        wlr_scene_node_set_position(&m_tree->node, 0, 0);
        wlr_scene_node_set_enabled(&m_tree->node, true);
        m_output.compositor().update_popup_positions();
    }

    void Workspace::deactivate() noexcept {
        clear_slide_clips();
        wlr_scene_node_set_enabled(&m_tree->node, false);
        wlr_scene_node_set_position(&m_tree->node, 0, 0);
        prepare_slide();
    }

    void Workspace::prepare_slide() noexcept {
        snap_animations();
        restack();
        wlr_ext_workspace_handle_v1_set_active(m_handle, false);
    }

    void Workspace::apply_slide_clip(int offset_x) noexcept {
        const wlr_box out_box = m_output.layout_box();
        for (Window* w : m_windows) {
            w->set_slide_clip(out_box, offset_x);
        }
    }

    void Workspace::clear_slide_clips() noexcept {
        for (Window* w : m_windows) {
            w->clear_slide_clip();
        }
    }

    void Workspace::snap_animations() noexcept {
        for (Window* w : m_windows) {
            w->snap_animation();
        }
        snap_closing_animations();
    }

    void Workspace::promote(Window* window) {
        if (!window) {
            return;
        }
        if (!std::ranges::contains(m_windows, window)) {
            m_windows.push_back(window);
            if (window->is_urgent()) {
                update_urgency();
            }
        }
        if (!window->is_floating()) {
            m_active_fullscreen = window->is_fullscreen() ? window : nullptr;
        }
        if (focused_window() == window) {
            return;
        }
        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);
    }

    void Workspace::remove(Window* window) noexcept {
        if (!window) {
            return;
        }
        if (m_active_fullscreen == window) {
            m_active_fullscreen = nullptr;
        }
        window->set_tiled_resizing(false);
        std::erase(m_focus_stack, window);
        std::erase(m_windows, window);
        update_urgency();
        const size_t remaining_non_floating =
            static_cast<size_t>(std::ranges::count_if(m_windows, [](const Window* w) { return !w->is_floating(); }));
        m_layout->on_window_removed(window, remaining_non_floating);
        m_output.compositor().input().cursor().cancel_grab(window);
    }

    void Workspace::update_urgency() noexcept {
        if (!m_handle) {
            return;
        }
        const bool urgent = std::ranges::any_of(m_windows, [](const Window* w) { return w->is_urgent(); });
        wlr_ext_workspace_handle_v1_set_urgent(m_handle, urgent);
    }

    Window* Workspace::cycle() {
        if (m_focus_stack.size() < 2) {
            return nullptr;
        }
        Window* initial = m_focus_stack.front();
        for (size_t i = 0; i < m_focus_stack.size(); ++i) {
            std::ranges::rotate(m_focus_stack, m_focus_stack.begin() + 1);
            if (resolve_modal(m_focus_stack.front()) == m_focus_stack.front()) {
                return m_focus_stack.front() != initial ? m_focus_stack.front() : nullptr;
            }
        }
        return nullptr;
    }

    Window* Workspace::resolve_modal(Window* window) const noexcept {
        if (!window) {
            return nullptr;
        }
        Window* cur = window;
        for (size_t depth = 0; depth < m_focus_stack.size(); ++depth) {
            auto it = std::ranges::find_if(
                m_focus_stack, [cur](const Window* w) { return w != cur && w->is_modal() && w->parent() == cur; });
            if (it == m_focus_stack.end()) {
                break;
            }
            cur = *it;
        }
        return cur;
    }

    void Workspace::sync_layout() {
        const Layout mode = m_output.compositor().config().layout().mode;
        if (m_layout->mode() == mode) {
            return;
        }
        if (is_resizing()) {
            m_output.compositor().input().cursor().cancel_grab();
        }
        m_layout = layout::create(mode);
    }

    layout::Context Workspace::layout_context() const noexcept {
        return {
            .windows = m_windows,
            .focus_stack = m_focus_stack,
            .usable = m_output.usable_area(),
            .config = m_output.compositor().config().layout(),
        };
    }

    void Workspace::arrange() {
        sync_layout();
        const auto& layout_cfg = m_output.compositor().config().layout();
        const wlr_box full = m_output.layout_box();
        const wlr_box& usable = m_output.usable_area();

        for (Window* w : m_windows) {
            const wlr_box& bounds = w->is_fullscreen() ? full : usable;
            w->set_bounds(bounds.width, bounds.height);
            if (w->is_fullscreen()) {
                w->set_geometry(full);
            } else if (w->is_maximized()) {
                const int bw = layout_cfg.gaps.smart ? 0 : layout_cfg.border.width;
                w->set_geometry(layout::inset_border(usable, bw));
            } else if (w->is_floating()) {
                if (!w->is_placed() && !w->is_pending_center()) {
                    w->center(usable);
                }
                w->update_borders();
            }
        }

        m_layout->arrange(layout_context());
    }

    void Workspace::restack() {
        const bool is_busy_output = m_output.animation().is_busy();
        Window* focused = resolve_modal(focused_window());
        Window* top_fullscreen = (focused && focused->is_fullscreen()) ? focused : nullptr;

        if (m_active_fullscreen && !m_active_fullscreen->is_fullscreen()) {
            m_active_fullscreen = nullptr;
        }
        if (focused && !focused->is_floating()) {
            m_active_fullscreen = top_fullscreen;
        }
        if (!m_active_fullscreen && std::ranges::none_of(m_focus_stack, [](const Window* w) {
                return !w->is_fullscreen() && !w->is_floating();
            })) {
            auto it = std::ranges::find_if(m_focus_stack, &Window::is_fullscreen);
            if (it != m_focus_stack.end()) {
                m_active_fullscreen = *it;
            }
        }

        const bool is_active_ws = &m_output.active_workspace() == this;
        const bool in_fullscreen_layer = is_active_ws && !is_busy_output;

        for (Window* w : m_focus_stack) {
            if (w->is_fullscreen()) {
                if (w == top_fullscreen && in_fullscreen_layer) {
                    wlr_scene_node_reparent(&w->scene_tree()->node, m_output.compositor().scene().fullscreen());
                } else {
                    w->reparent_to_workspace();
                }
                const bool should_show = (w == m_active_fullscreen);
                if (!should_show) {
                    w->clear_slide_clip();
                }
                wlr_scene_node_set_enabled(&w->scene_tree()->node, should_show && !w->is_slide_hidden());
            }
        }

        wlr_scene_node* prev = nullptr;
        wlr_xwayland_surface* prev_xsurf = nullptr;
        auto place_node = [&](wlr_scene_node* node, bool advance_prev = true) {
            if (!prev) {
                wlr_scene_node_raise_to_top(node);
            } else {
                wlr_scene_node_place_below(node, prev);
            }
            if (advance_prev) {
                prev = node;
            }
        };
        auto place = [&](Window* w) {
            place_node(&w->scene_tree()->node, w != top_fullscreen || !in_fullscreen_layer);
            if (is_active_ws) {
                if (auto* xsurf = w->xwayland_surface()) {
                    wlr_xwayland_surface_restack(xsurf, prev_xsurf,
                                                 prev_xsurf ? XCB_STACK_MODE_BELOW : XCB_STACK_MODE_ABOVE);
                    prev_xsurf = xsurf;
                }
            }
        };

        if (top_fullscreen) {
            place(top_fullscreen);
        }

        for (const auto& cw : m_closing_windows) {
            if (cw->tree() && cw->is_floating()) {
                place_node(&cw->tree()->node);
            }
        }

        const bool has_floating_modal =
            std::ranges::any_of(m_focus_stack, [](const Window* w) { return w->is_floating() && w->is_modal(); });
        if (!has_floating_modal) {
            for (Window* w : m_focus_stack) {
                if (w->is_floating()) {
                    place(w);
                }
            }
        } else {
            std::vector<Window*> floating;
            floating.reserve(m_focus_stack.size());
            for (Window* w : m_focus_stack) {
                if (w->is_floating()) {
                    floating.push_back(w);
                }
            }
            bool changed = true;
            for (size_t pass = 0; changed && pass < floating.size(); ++pass) {
                changed = false;
                for (size_t i = 0; i < floating.size(); ++i) {
                    const Window* w = floating[i];
                    if (!w->is_modal()) {
                        continue;
                    }
                    const Window* parent = w->parent();
                    const auto cur_it = floating.begin() + static_cast<std::ptrdiff_t>(i);
                    const auto parent_it = std::ranges::find(floating.begin(), cur_it, parent);
                    if (parent_it != cur_it) {
                        std::rotate(parent_it, cur_it, cur_it + 1);
                        changed = true;
                    }
                }
            }
            for (Window* w : floating) {
                place(w);
            }
        }

        if (m_active_fullscreen && m_active_fullscreen != top_fullscreen) {
            place(m_active_fullscreen);
        }

        for (const auto& cw : m_closing_windows) {
            if (cw->tree() && !cw->is_floating()) {
                place_node(&cw->tree()->node);
            }
        }

        for (Window* w : m_focus_stack) {
            if (!w->is_floating() && !w->is_fullscreen()) {
                place(w);
            }
        }

        if (is_busy_output && m_tree && m_tree->node.enabled && m_output.compositor().outputs().size() > 1) {
            apply_slide_clip(m_tree->node.x);
        }

        m_output.compositor().update_popup_positions();
    }

    Window* Workspace::window_in_direction(Direction direction) const {
        Window* focused = focused_window();
        if (!focused || focused->is_fullscreen() || focused->is_maximized()) {
            return nullptr;
        }

        if (focused->is_floating()) {
            if (m_active_fullscreen) {
                return m_active_fullscreen;
            }
            auto bg_it =
                std::ranges::find_if(m_focus_stack, [](const Window* w) { return w->is_tiled() || w->is_maximized(); });
            if (bg_it == m_focus_stack.end()) {
                return nullptr;
            }
            if ((*bg_it)->is_maximized()) {
                return *bg_it;
            }
            return m_layout->floating_fallback(layout_context(), direction, *bg_it);
        }

        return m_layout->window_in_direction(layout_context(), focused, direction,
                                             m_output.adjacent_output(direction) != nullptr);
    }

    size_t Workspace::tiled_count() const noexcept {
        return static_cast<size_t>(std::ranges::count_if(m_windows, &Window::is_tiled));
    }

    wlr_box Workspace::next_tile_box() const noexcept {
        return m_layout->next_tile_box(layout_context());
    }

    bool Workspace::is_solitary() const noexcept {
        return m_layout->is_solitary(tiled_count());
    }

    bool Workspace::can_tiled_resize() const noexcept {
        return m_layout->can_resize(tiled_count());
    }

    void Workspace::add_closing_window(Window* window, double initial_progress) {
        if (!window || window->is_fullscreen() || initial_progress <= 0.01 || &m_output.active_workspace() != this) {
            return;
        }

        const auto& comp = m_output.compositor();
        if (!comp.config().animations().enabled || comp.is_locked() || comp.is_workspace_swiping() ||
            !m_output.is_enabled() || m_output.animation().is_busy()) {
            return;
        }
        if (!window->is_floating() &&
            ((m_active_fullscreen != nullptr) ||
             (m_layout->hides_unfocused() && window != focused_window() && tiled_count() > 1))) {
            return;
        }

        auto closing = ClosingWindow::capture(m_tree, *window, comp.config().layout().border, initial_progress);
        if (!closing) {
            return;
        }

        m_closing_windows.push_back(std::move(closing));
        if (m_output.raw()) {
            wlr_output_schedule_frame(m_output.raw());
        }
    }

    bool Workspace::step_closing_animations(TimePoint now) noexcept {
        if (m_closing_windows.empty()) {
            return false;
        }

        const double slowdown = m_output.compositor().config().animations().slowdown;
        std::erase_if(m_closing_windows, [&](auto& cw) { return !cw->step(now, slowdown); });

        return !m_closing_windows.empty();
    }

    void Workspace::snap_closing_animations() noexcept {
        m_closing_windows.clear();
    }

    std::unique_ptr<layout::ResizeSession> Workspace::begin_tiled_resize(Window* window, double cursor_x,
                                                                         double cursor_y) {
        return m_layout->begin_resize(layout_context(), window, cursor_x, cursor_y);
    }

    bool Workspace::is_resizing() const noexcept {
        return m_output.compositor().input().cursor().is_tiled_resizing(this);
    }

    void Workspace::swap_windows(Window* a, Window* b) {
        if (!a || !b || a == b) {
            return;
        }
        const auto it_a = std::ranges::find(m_windows, a);
        const auto it_b = std::ranges::find(m_windows, b);
        if (it_a == m_windows.end() || it_b == m_windows.end()) {
            return;
        }

        m_layout->on_windows_swapped(a, b);
        std::iter_swap(it_a, it_b);
        arrange();
    }

    void Workspace::swap_tiled_at(Window* window, double cursor_x, double cursor_y) {
        if (!window) {
            return;
        }
        if (Window* target = m_layout->window_at(layout_context(), cursor_x, cursor_y); target && target != window) {
            swap_windows(window, target);
        }
    }

    bool Workspace::swap_window_in_direction(Direction direction) {
        Window* focused = focused_window();
        if (!focused || !focused->is_tiled()) {
            return false;
        }

        Window* target = m_layout->swap_target_in_direction(layout_context(), focused, direction);
        if (!target || target == focused) {
            return false;
        }

        m_output.compositor().input().cursor().cancel_grab();
        m_output.compositor().dismiss_popups(focused->scene_tree());
        swap_windows(focused, target);
        return true;
    }

}
