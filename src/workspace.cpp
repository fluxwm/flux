#include "frost/workspace.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/input.hpp"
#include "frost/output.hpp"
#include "frost/scene.hpp"
#include "frost/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>

namespace frost {

    namespace {

        [[nodiscard]] constexpr wlr_box inset_border(const wlr_box& cell, int bw) noexcept {
            return {
                .x = cell.x + bw,
                .y = cell.y + bw,
                .width = std::max(1, cell.width - (2 * bw)),
                .height = std::max(1, cell.height - (2 * bw)),
            };
        }

        [[nodiscard]] LayoutMetrics compute_layout_metrics(const wlr_box& usable, size_t total,
                                                           const LayoutConfig& layout, float master_factor,
                                                           float raw_stack_weight) noexcept {
            const size_t effective_total = (layout.mode == Layout::Monocle) ? std::min<size_t>(total, 1) : total;
            const bool solitary = (effective_total <= 1);
            const auto& gaps = layout.gaps;
            const int outer = (solitary && gaps.smart) ? 0 : gaps.outer;

            LayoutMetrics m{
                .tiled_count = total,
                .area_x = usable.x + outer,
                .area_y = usable.y + outer,
                .area_w = std::max(1, usable.width - (2 * outer)),
                .area_h = std::max(1, usable.height - (2 * outer)),
                .inner_gap = gaps.inner,
                .border_width = (solitary && gaps.smart) ? 0 : layout.border.width,
                .total_stack_weight = raw_stack_weight,
            };

            if (!solitary) {
                m.avail_w = std::max(1, m.area_w - gaps.inner);
                const int min_w = std::min(50, m.avail_w / 2);
                m.master_w = std::max(
                    1, std::clamp(static_cast<int>(std::round(m.avail_w * master_factor)), min_w, m.avail_w - min_w));
                m.stack_x = m.area_x + m.master_w + gaps.inner;
                m.stack_w = std::max(1, m.avail_w - m.master_w);
                m.avail_h = std::max(1, m.area_h - (static_cast<int>(effective_total - 2) * gaps.inner));
            }
            return m;
        }

        [[nodiscard]] int stack_slot_height(int avail_h, float weight, float total_weight) noexcept {
            return std::max(1, static_cast<int>(std::round(avail_h * (weight / total_weight))));
        }

        template <typename Fn>
        void for_each_tiled_slot(std::span<Window* const> windows, const LayoutMetrics& m, Layout mode, const Fn& fn) {
            const bool single_cell = (mode == Layout::Monocle) || (m.tiled_count <= 1);
            const float effective_weight = m.effective_stack_weight();

            size_t idx = 0;
            int cur_y = m.area_y;
            for (Window* w : windows) {
                if (!w->is_tiled()) {
                    continue;
                }
                wlr_box cell{};
                if (single_cell || idx == 0) {
                    cell = {
                        .x = m.area_x,
                        .y = m.area_y,
                        .width = single_cell ? m.area_w : m.master_w,
                        .height = m.area_h,
                    };
                } else {
                    const int stack_h = stack_slot_height(m.avail_h, w->tile_weight(), effective_weight);
                    const int height =
                        (idx == m.tiled_count - 1) ? std::max(1, (m.area_y + m.area_h) - cur_y) : stack_h;
                    cell = {.x = m.stack_x, .y = cur_y, .width = m.stack_w, .height = height};
                    cur_y += stack_h + m.inner_gap;
                }
                if (!fn(w, inset_border(cell, m.border_width))) {
                    return;
                }
                ++idx;
            }
        }

    }

    Workspace::Workspace(uint32_t id, Output& output, Scene& scene, wlr_ext_workspace_group_handle_v1* group)
        : m_id(id), m_output(output), m_tree(wlr_scene_tree_create(scene.windows())) {
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
        window->set_tile_weight(1.0f);
        std::erase(m_focus_stack, window);
        std::erase(m_windows, window);
        const size_t layout_count =
            static_cast<size_t>(std::ranges::count_if(m_windows, [](const Window* w) { return !w->is_floating(); }));
        if (layout_count <= 2) {
            for (Window* w : m_windows) {
                w->set_tile_weight(1.0f);
            }
        }
        if (layout_count <= 1) {
            m_master_factor = 0.5f;
        }
        m_output.compositor().input().cursor().cancel_grab(window);
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

    void Workspace::arrange() {
        const auto& layout_cfg = m_output.compositor().config().layout();
        const wlr_box full = m_output.layout_box();
        const wlr_box& usable = m_output.usable_area();

        size_t tiled_count = 0;
        float raw_stack_weight = 0.0f;
        for (Window* w : m_windows) {
            const wlr_box& bounds = w->is_fullscreen() ? full : usable;
            w->set_bounds(bounds.width, bounds.height);
            if (w->is_fullscreen()) {
                w->set_geometry(full);
            } else if (w->is_maximized()) {
                const int bw = layout_cfg.gaps.smart ? 0 : layout_cfg.border.width;
                w->set_geometry({
                    .x = usable.x + bw,
                    .y = usable.y + bw,
                    .width = std::max(1, usable.width - (2 * bw)),
                    .height = std::max(1, usable.height - (2 * bw)),
                });
            } else if (w->is_floating()) {
                if (!w->is_placed() && !w->is_pending_center()) {
                    w->center(usable);
                }
                w->update_borders();
            } else {
                if (tiled_count > 0) {
                    raw_stack_weight += w->tile_weight();
                }
                ++tiled_count;
            }
        }

        if (tiled_count > 0) {
            const LayoutMetrics metrics =
                compute_layout_metrics(usable, tiled_count, layout_cfg, m_master_factor, raw_stack_weight);
            for_each_tiled_slot(m_windows, metrics, layout_cfg.mode, [](Window* w, const wlr_box& box) {
                w->set_geometry(box);
                return true;
            });
        }
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

    Window* Workspace::top_stack_window(const Window* master, Window* fallback) const noexcept {
        auto it = std::ranges::find_if(m_focus_stack, [&](const Window* w) { return w != master && w->is_tiled(); });
        return (it != m_focus_stack.end()) ? *it : fallback;
    }

    Window* Workspace::window_in_direction(Direction direction) const {
        Window* focused = focused_window();
        if (!focused || focused->is_fullscreen() || focused->is_maximized()) {
            return nullptr;
        }

        const TiledNeighbors n = tiled_neighbors(focused);
        const bool is_monocle = m_output.compositor().config().layout().mode == Layout::Monocle;

        if (focused->is_floating()) {
            if (m_active_fullscreen) {
                return m_active_fullscreen;
            }
            auto bg_it =
                std::ranges::find_if(m_focus_stack, [](const Window* w) { return w->is_tiled() || w->is_maximized(); });
            if (bg_it == m_focus_stack.end()) {
                return nullptr;
            }
            if ((*bg_it)->is_maximized() || n.count <= 1 || is_monocle) {
                return *bg_it;
            }
            if (direction == Direction::Left) {
                return n.master;
            }
            if (direction == Direction::Right) {
                return top_stack_window(n.master, n.first_stack);
            }
            return *bg_it;
        }

        if (n.count <= 1 || !n.found) {
            return nullptr;
        }

        auto wrap = [&](Window* target) -> Window* { return m_output.adjacent_output(direction) ? nullptr : target; };

        if (is_monocle) {
            const bool at_edge = (direction == Direction::Left || direction == Direction::Right) ||
                                 (direction == Direction::Up && n.index == 0) ||
                                 (direction == Direction::Down && n.index == n.count - 1);
            Window* target = (direction == Direction::Down || direction == Direction::Right)
                                 ? (n.next ? n.next : n.master)
                                 : (n.prev ? n.prev : n.last);
            return at_edge ? wrap(target) : target;
        }

        if (n.index == 0) {
            switch (direction) {
                case Direction::Right:
                    return top_stack_window(n.master, n.first_stack);
                case Direction::Left:
                    return nullptr;
                case Direction::Down:
                    return wrap(n.first_stack);
                case Direction::Up:
                    return wrap(n.last);
            }
        }

        switch (direction) {
            case Direction::Left:
                return n.master;
            case Direction::Right:
                return nullptr;
            case Direction::Up:
                return (n.index == 1) ? wrap(n.last) : n.prev;
            case Direction::Down:
                return (n.index == n.count - 1) ? wrap(n.first_stack) : n.next;
        }
        return nullptr;
    }

    LayoutMetrics Workspace::layout_metrics() const noexcept {
        size_t count = 0;
        Window* master = nullptr;
        float raw_stack_weight = 0.0f;
        for (Window* w : m_windows) {
            if (!w->is_tiled()) {
                continue;
            }
            if (count == 0) {
                master = w;
            } else {
                raw_stack_weight += w->tile_weight();
            }
            ++count;
        }
        LayoutMetrics m = compute_layout_metrics(m_output.usable_area(), count, m_output.compositor().config().layout(),
                                                 m_master_factor, raw_stack_weight);
        m.master = master;
        return m;
    }

    TiledNeighbors Workspace::tiled_neighbors(const Window* target) const noexcept {
        TiledNeighbors n;
        for (Window* w : m_windows) {
            if (!w->is_tiled()) {
                continue;
            }
            if (n.count == 0) {
                n.master = w;
            } else if (n.count == 1) {
                n.first_stack = w;
            }
            if (w == target) {
                n.found = true;
                n.index = n.count;
                n.prev = n.last;
            } else if (n.found && !n.next) {
                n.next = w;
            }
            n.last = w;
            ++n.count;
        }
        return n;
    }

    size_t Workspace::tiled_count() const noexcept {
        return static_cast<size_t>(std::ranges::count_if(m_windows, &Window::is_tiled));
    }

    wlr_box Workspace::next_tile_box() const noexcept {
        const auto& layout = m_output.compositor().config().layout();
        const auto& usable = m_output.usable_area();
        const LayoutMetrics cur = (layout.mode == Layout::Monocle) ? LayoutMetrics{} : layout_metrics();
        if (cur.tiled_count == 0) {
            const LayoutMetrics m = compute_layout_metrics(usable, 1, layout, m_master_factor, 0.0f);
            return inset_border({.x = m.area_x, .y = m.area_y, .width = m.area_w, .height = m.area_h}, m.border_width);
        }

        const LayoutMetrics m =
            compute_layout_metrics(usable, cur.tiled_count + 1, layout, m_master_factor, cur.total_stack_weight + 1.0f);
        const float effective_weight = m.effective_stack_weight();

        int cur_y = m.area_y;
        if (cur.tiled_count > 1) {
            size_t idx = 0;
            for (Window* w : m_windows) {
                if (!w->is_tiled()) {
                    continue;
                }
                if (idx > 0) {
                    cur_y += stack_slot_height(m.avail_h, w->tile_weight(), effective_weight) + m.inner_gap;
                }
                ++idx;
            }
        }

        const int height = std::max(1, (m.area_y + m.area_h) - cur_y);
        return inset_border({.x = m.stack_x, .y = cur_y, .width = m.stack_w, .height = height}, m.border_width);
    }

    bool Workspace::is_solitary() const noexcept {
        return m_output.compositor().config().layout().mode == Layout::Monocle || tiled_count() <= 1;
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
             (comp.config().layout().mode == Layout::Monocle && window != focused_window() && tiled_count() > 1))) {
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

    uint32_t Workspace::tiled_resize_edges(const Window* window, double cursor_x, double cursor_y) const noexcept {
        if (!window || m_output.compositor().config().layout().mode == Layout::Monocle) {
            return WLR_EDGE_NONE;
        }
        const TiledNeighbors n = tiled_neighbors(window);
        if (n.count <= 1 || !n.found) {
            return WLR_EDGE_NONE;
        }
        if (n.index == 0) {
            return WLR_EDGE_RIGHT;
        }

        const size_t stack_idx = n.index - 1;
        const size_t stack_count = n.count - 1;
        if (stack_count <= 1) {
            return WLR_EDGE_LEFT;
        }

        const wlr_box box = window->geometry_box();
        uint32_t vert_edge = WLR_EDGE_NONE;
        if (stack_idx == 0) {
            vert_edge = WLR_EDGE_BOTTOM;
        } else if (stack_idx == stack_count - 1) {
            vert_edge = WLR_EDGE_TOP;
        } else {
            vert_edge = (cursor_y < box.y + (box.height / 2.0)) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
        }

        const double rel_x = cursor_x - box.x;
        const bool near_master_split = rel_x < std::max(40.0, box.width * 0.25);
        if (!near_master_split) {
            return vert_edge;
        }

        double split_y = box.y;
        if (vert_edge == WLR_EDGE_BOTTOM) {
            split_y = box.y + box.height;
        }
        const bool near_stack_split = std::abs(cursor_y - split_y) < std::max(40.0, box.height * 0.35);
        if (near_stack_split) {
            return WLR_EDGE_LEFT | vert_edge;
        }
        return WLR_EDGE_LEFT;
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

        const float weight_a = a->tile_weight();
        a->set_tile_weight(b->tile_weight());
        b->set_tile_weight(weight_a);
        std::iter_swap(it_a, it_b);
        arrange();
    }

    void Workspace::swap_tiled_at(Window* window, double cursor_x, double cursor_y) {
        const auto& layout_cfg = m_output.compositor().config().layout();
        if (!window || layout_cfg.mode == Layout::Monocle) {
            return;
        }
        const LayoutMetrics metrics = layout_metrics();
        if (metrics.tiled_count <= 1) {
            return;
        }

        for_each_tiled_slot(m_windows, metrics, layout_cfg.mode, [&](Window* target, const wlr_box& slot) {
            if (wlr_box_contains_point(&slot, cursor_x, cursor_y)) {
                if (target != window) {
                    swap_windows(window, target);
                }
                return false;
            }
            return true;
        });
    }

    bool Workspace::swap_window_in_direction(Direction direction) {
        if (m_output.compositor().config().layout().mode == Layout::Monocle) {
            return false;
        }

        Window* focused = focused_window();
        if (!focused || !focused->is_tiled()) {
            return false;
        }

        const TiledNeighbors n = tiled_neighbors(focused);
        if (n.count <= 1 || !n.found) {
            return false;
        }

        Window* target = nullptr;
        if (n.index == 0) {
            switch (direction) {
                case Direction::Right:
                    target = top_stack_window(n.master, n.first_stack);
                    break;
                case Direction::Left:
                case Direction::Up:
                case Direction::Down:
                    return false;
            }
        } else {
            switch (direction) {
                case Direction::Left:
                    target = n.master;
                    break;
                case Direction::Right:
                    return false;
                case Direction::Up:
                    if (n.index > 1) {
                        target = n.prev;
                    }
                    break;
                case Direction::Down:
                    target = n.next;
                    break;
            }
        }

        if (!target) {
            return false;
        }

        m_output.compositor().input().cursor().cancel_grab();
        m_output.compositor().dismiss_popups(focused->scene_tree());
        swap_windows(focused, target);
        return true;
    }

}
