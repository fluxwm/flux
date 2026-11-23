#include "frost/workspace.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/input.hpp"
#include "frost/output.hpp"
#include "frost/scene.hpp"
#include "frost/window.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace frost {

    namespace {

        [[nodiscard]] wlr_box tile_box(const wlr_box& usable, size_t index, size_t total, const LayoutConfig& layout,
                                       float master_factor, std::span<Window* const> tiled) noexcept {
            if (layout.mode == Layout::Monocle) {
                total = 1;
            }

            const auto& gaps = layout.gaps;

            const int outer = (total <= 1 && gaps.smart) ? 0 : gaps.outer;
            const int area_x = usable.x + outer;
            const int area_y = usable.y + outer;
            const int area_w = std::max(1, usable.width - (2 * outer));
            const int area_h = std::max(1, usable.height - (2 * outer));
            wlr_box cell{};
            if (total <= 1) {
                cell = {.x = area_x, .y = area_y, .width = area_w, .height = area_h};
            } else {
                const int avail_w = std::max(1, area_w - gaps.inner);
                const int min_w = std::min(50, avail_w / 2);
                const int master_w = std::max(
                    1, std::clamp(static_cast<int>(std::round(avail_w * master_factor)), min_w, avail_w - min_w));
                if (index == 0) {
                    cell = {.x = area_x, .y = area_y, .width = master_w, .height = area_h};
                } else {
                    const int stack_x = area_x + master_w + gaps.inner;
                    const int stack_w = std::max(1, avail_w - master_w);
                    const size_t stack_count = total - 1;
                    const int avail_h = std::max(1, area_h - (static_cast<int>(stack_count - 1) * gaps.inner));

                    float total_weight = 0.0f;
                    for (size_t i = 1; i < total; ++i) {
                        total_weight += (i < tiled.size()) ? tiled[i]->tile_weight() : 1.0f;
                    }
                    if (total_weight <= 0.0f) {
                        total_weight = static_cast<float>(stack_count);
                    }

                    int cur_y = area_y;
                    int stack_h = 0;
                    for (size_t i = 1; i <= index && i < total; ++i) {
                        const float w = (i < tiled.size()) ? tiled[i]->tile_weight() : 1.0f;
                        stack_h = std::max(1, static_cast<int>(std::round(avail_h * (w / total_weight))));
                        if (i < index) {
                            cur_y += stack_h + gaps.inner;
                        }
                    }

                    const int height = (index == total - 1) ? std::max(1, (area_y + area_h) - cur_y) : stack_h;
                    cell = {.x = stack_x, .y = cur_y, .width = stack_w, .height = height};
                }
            }

            const bool smart_border = (total <= 1 && gaps.smart);
            const int bw = smart_border ? 0 : layout.border.width;
            return {
                .x = cell.x + bw,
                .y = cell.y + bw,
                .width = std::max(1, cell.width - (2 * bw)),
                .height = std::max(1, cell.height - (2 * bw)),
            };
        }

    }

    Workspace::Workspace(uint32_t id, Output& output, Scene& scene, wlr_ext_workspace_group_handle_v1* group)
        : m_id(id), m_output(output), m_tree(wlr_scene_tree_create(scene.windows())) {
        wlr_scene_node_set_enabled(&m_tree->node, false);

        const std::string id_str = std::to_string(id);
        constexpr uint32_t caps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
        m_handle = wlr_ext_workspace_handle_v1_create(group->manager, id_str.c_str(), caps);
        m_handle->data = this;
        wlr_ext_workspace_handle_v1_set_group(m_handle, group);
        wlr_ext_workspace_handle_v1_set_name(m_handle, id_str.c_str());
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
        wlr_scene_node_set_position(&m_tree->node, 0, 0);
        wlr_scene_node_set_enabled(&m_tree->node, true);
        m_output.compositor().update_popup_positions();
    }

    void Workspace::deactivate() noexcept {
        wlr_scene_node_set_enabled(&m_tree->node, false);
        wlr_scene_node_set_position(&m_tree->node, 0, 0);
        prepare_slide();
    }

    void Workspace::prepare_slide() noexcept {
        snap_animations();
        restack();
        wlr_ext_workspace_handle_v1_set_active(m_handle, false);
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
        std::ranges::rotate(m_focus_stack, m_focus_stack.begin() + 1);
        return m_focus_stack.front();
    }

    void Workspace::arrange() {
        const auto& layout_cfg = m_output.compositor().config().layout();
        const wlr_box full = m_output.layout_box();
        const wlr_box& usable = m_output.usable_area();

        std::vector<Window*> tiled;
        for (Window* w : m_windows) {
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
                tiled.push_back(w);
            }
        }

        for (size_t i = 0; i < tiled.size(); ++i) {
            tiled[i]->set_geometry(tile_box(usable, i, tiled.size(), layout_cfg, m_master_factor, tiled));
        }
    }

    void Workspace::restack() {
        const bool is_busy_output = m_output.animation().is_busy();
        Window* focused = focused_window();
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
                wlr_scene_node_set_enabled(&w->scene_tree()->node, w == m_active_fullscreen);
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
            if (cw->tree && cw->is_floating) {
                place_node(&cw->tree->node);
            }
        }

        for (Window* w : m_focus_stack) {
            if (w->is_floating()) {
                place(w);
            }
        }

        if (m_active_fullscreen && m_active_fullscreen != top_fullscreen) {
            place(m_active_fullscreen);
        }

        for (const auto& cw : m_closing_windows) {
            if (cw->tree && !cw->is_floating) {
                place_node(&cw->tree->node);
            }
        }

        for (Window* w : m_focus_stack) {
            if (!w->is_floating() && !w->is_fullscreen()) {
                place(w);
            }
        }

        m_output.compositor().update_popup_positions();
    }

    Window* Workspace::window_in_direction(Direction direction) const {
        Window* focused = focused_window();
        if (!focused || focused->is_fullscreen() || focused->is_maximized()) {
            return nullptr;
        }

        const std::vector<Window*> tiled = tiled_windows();
        const bool is_monocle = m_output.compositor().config().layout().mode == Layout::Monocle;
        auto top_stack_window = [&] -> Window* {
            auto stack_it =
                std::ranges::find_if(m_focus_stack, [&](const Window* w) { return w != tiled[0] && w->is_tiled(); });
            return (stack_it != m_focus_stack.end()) ? *stack_it : tiled[1];
        };

        if (focused->is_floating()) {
            if (m_active_fullscreen) {
                return m_active_fullscreen;
            }
            auto bg_it =
                std::ranges::find_if(m_focus_stack, [](const Window* w) { return w->is_tiled() || w->is_maximized(); });
            if (bg_it == m_focus_stack.end()) {
                return nullptr;
            }
            if ((*bg_it)->is_maximized() || tiled.size() <= 1 || is_monocle) {
                return *bg_it;
            }
            if (direction == Direction::Left) {
                return tiled[0];
            }
            if (direction == Direction::Right) {
                return top_stack_window();
            }
            return *bg_it;
        }

        if (tiled.size() <= 1) {
            return nullptr;
        }

        const auto it = std::ranges::find(tiled, focused);
        if (it == tiled.end()) {
            return nullptr;
        }
        const size_t idx = static_cast<size_t>(std::distance(tiled.begin(), it));
        auto wrap = [&](Window* target) -> Window* { return m_output.adjacent_output(direction) ? nullptr : target; };

        if (is_monocle) {
            const bool at_edge = (direction == Direction::Left || direction == Direction::Right) ||
                                 (direction == Direction::Up && idx == 0) ||
                                 (direction == Direction::Down && idx == tiled.size() - 1);
            const size_t next = (direction == Direction::Down || direction == Direction::Right)
                                    ? ((idx + 1) % tiled.size())
                                    : ((idx == 0) ? (tiled.size() - 1) : (idx - 1));
            return at_edge ? wrap(tiled[next]) : tiled[next];
        }

        if (idx == 0) {
            switch (direction) {
                case Direction::Right:
                    return top_stack_window();
                case Direction::Left:
                    return nullptr;
                case Direction::Down:
                    return wrap(tiled[1]);
                case Direction::Up:
                    return wrap(tiled.back());
            }
        }

        switch (direction) {
            case Direction::Left:
                return tiled[0];
            case Direction::Right:
                return nullptr;
            case Direction::Up:
                return (idx == 1) ? wrap(tiled.back()) : tiled[idx - 1];
            case Direction::Down:
                return (idx == tiled.size() - 1) ? wrap(tiled[1]) : tiled[idx + 1];
        }
        return nullptr;
    }

    std::vector<Window*> Workspace::tiled_windows() const {
        std::vector<Window*> tiled;
        for (Window* w : m_windows) {
            if (w->is_tiled()) {
                tiled.push_back(w);
            }
        }
        return tiled;
    }

    size_t Workspace::tiled_count() const noexcept {
        return static_cast<size_t>(std::ranges::count_if(m_windows, &Window::is_tiled));
    }

    wlr_box Workspace::next_tile_box() const noexcept {
        const auto& layout = m_output.compositor().config().layout();
        const std::vector<Window*> tiled = tiled_windows();
        const size_t count = (layout.mode == Layout::Monocle) ? 0 : tiled.size();
        return tile_box(m_output.usable_area(), count, count + 1, layout, m_master_factor, tiled);
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

        const wlr_box box = window->geometry_box();
        if (box.width <= 0 || box.height <= 0) {
            return;
        }

        auto closing = std::make_unique<ClosingWindow>();
        closing->tree = wlr_scene_tree_create(m_tree);
        if (!closing->tree) {
            return;
        }

        int win_lx = 0;
        int win_ly = 0;
        wlr_scene_node_coords(&window->scene_tree()->node, &win_lx, &win_ly);

        closing->box = box;
        closing->is_floating = window->is_floating();

        struct CloneContext {
            wlr_scene_tree* tree;
            int win_lx;
            int win_ly;
            std::vector<ClosingBuffer>* buffers;
        } ctx{
            .tree = closing->tree,
            .win_lx = win_lx,
            .win_ly = win_ly,
            .buffers = &closing->buffers,
        };

        wlr_scene_node_for_each_buffer(
            &window->scene_tree()->node,
            [](wlr_scene_buffer* src, int sx, int sy, void* data) {
                if (!src || !src->buffer) {
                    return;
                }
                auto* c = static_cast<CloneContext*>(data);
                wlr_scene_buffer* clone = wlr_scene_buffer_create(c->tree, src->buffer);
                if (!clone) {
                    return;
                }

                const int orig_w = src->dst_width > 0 ? src->dst_width : src->buffer->width;
                const int orig_h = src->dst_height > 0 ? src->dst_height : src->buffer->height;

                wlr_scene_buffer_set_source_box(clone, &src->src_box);
                wlr_scene_buffer_set_transform(clone, src->transform);
                wlr_scene_buffer_set_filter_mode(clone, src->filter_mode);
                clone->point_accepts_input = [](wlr_scene_buffer*, double*, double*) { return false; };

                c->buffers->push_back(ClosingBuffer{
                    .buffer = clone,
                    .rel_x = sx - c->win_lx,
                    .rel_y = sy - c->win_ly,
                    .width = orig_w,
                    .height = orig_h,
                    .base_opacity = src->opacity,
                });
            },
            &ctx);

        if (closing->buffers.empty()) {
            return;
        }

        if (window->has_visible_borders()) {
            const auto& border_cfg = comp.config().layout().border;
            closing->border_width = border_cfg.width;
            closing->border_color = window->is_activated() ? border_cfg.active_color : border_cfg.inactive_color;
            closing->border.init(closing->tree);
        }

        const double initial_p = std::clamp(initial_progress, 0.0, 1.0);
        closing->apply_transform(initial_p);
        closing->spring.reset(initial_p, 0.0);
        closing->spring.set_target(0.0);
        clock_gettime(CLOCK_MONOTONIC, &closing->last_time);

        m_closing_windows.push_back(std::move(closing));
        if (m_output.raw()) {
            wlr_output_schedule_frame(m_output.raw());
        }
    }

    bool Workspace::step_closing_animations(const timespec& now) noexcept {
        if (m_closing_windows.empty()) {
            return false;
        }

        const double slowdown = m_output.compositor().config().animations().slowdown;
        std::erase_if(m_closing_windows, [&](auto& cw) {
            const double dt = step_dt(cw->last_time, now, 0.05, slowdown);
            cw->last_time = now;
            cw->spring.advance(dt);

            if (cw->spring.is_settled() || cw->spring.value() <= 0.005) {
                return true;
            }

            cw->apply_transform(cw->spring.value());
            return false;
        });

        return !m_closing_windows.empty();
    }

    void Workspace::snap_closing_animations() noexcept {
        m_closing_windows.clear();
    }

    uint32_t Workspace::tiled_resize_edges(const Window* window, double cursor_x, double cursor_y) const noexcept {
        if (!window || m_output.compositor().config().layout().mode == Layout::Monocle) {
            return WLR_EDGE_NONE;
        }
        const std::vector<Window*> tiled = tiled_windows();
        if (tiled.size() <= 1) {
            return WLR_EDGE_NONE;
        }
        const auto it = std::ranges::find(tiled, window);
        if (it == tiled.end()) {
            return WLR_EDGE_NONE;
        }
        if (it == tiled.begin()) {
            return WLR_EDGE_RIGHT;
        }

        const size_t stack_idx = static_cast<size_t>(std::distance(tiled.begin(), it)) - 1;
        const size_t stack_count = tiled.size() - 1;
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
        if (!window || m_output.compositor().config().layout().mode == Layout::Monocle) {
            return;
        }
        const std::vector<Window*> tiled = tiled_windows();
        if (tiled.size() <= 1) {
            return;
        }

        const auto& usable = m_output.usable_area();
        const auto& layout_cfg = m_output.compositor().config().layout();

        for (size_t i = 0; i < tiled.size(); ++i) {
            const wlr_box slot = tile_box(usable, i, tiled.size(), layout_cfg, m_master_factor, tiled);
            if (wlr_box_contains_point(&slot, cursor_x, cursor_y)) {
                Window* target = tiled[i];
                if (target != window) {
                    swap_windows(window, target);
                }
                break;
            }
        }
    }

    bool Workspace::swap_window_in_direction(Direction direction) {
        if (m_output.compositor().config().layout().mode == Layout::Monocle) {
            return false;
        }

        Window* focused = focused_window();
        if (!focused || !focused->is_tiled()) {
            return false;
        }

        const std::vector<Window*> tiled = tiled_windows();
        if (tiled.size() <= 1) {
            return false;
        }

        const auto it = std::ranges::find(tiled, focused);
        if (it == tiled.end()) {
            return false;
        }
        const size_t idx = static_cast<size_t>(std::distance(tiled.begin(), it));

        Window* target = nullptr;
        if (idx == 0) {
            switch (direction) {
                case Direction::Right: {
                    auto stack_it = std::ranges::find_if(
                        m_focus_stack, [&](const Window* w) { return w != tiled[0] && w->is_tiled(); });
                    target = (stack_it != m_focus_stack.end()) ? *stack_it : tiled[1];
                    break;
                }
                case Direction::Left:
                case Direction::Up:
                case Direction::Down:
                    return false;
            }
        } else {
            switch (direction) {
                case Direction::Left:
                    target = tiled[0];
                    break;
                case Direction::Right:
                    return false;
                case Direction::Up:
                    if (idx > 1) {
                        target = tiled[idx - 1];
                    }
                    break;
                case Direction::Down:
                    if (idx + 1 < tiled.size()) {
                        target = tiled[idx + 1];
                    }
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
