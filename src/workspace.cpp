#include "flux/workspace.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/output.hpp"
#include "flux/scene.hpp"
#include "flux/window.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace flux {

    namespace {

        [[nodiscard]] constexpr wlr_box tile_box(const wlr_box& usable, size_t index, size_t total,
                                                 const LayoutConfig& layout) noexcept {
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
                const int master_w = std::max(1, avail_w / 2);
                if (index == 0) {
                    cell = {.x = area_x, .y = area_y, .width = master_w, .height = area_h};
                } else {
                    const int stack_x = area_x + master_w + gaps.inner;
                    const int stack_w = std::max(1, avail_w - master_w);
                    const size_t stack_count = total - 1;
                    const int avail_h = std::max(1, area_h - (static_cast<int>(stack_count - 1) * gaps.inner));
                    const int stack_h = std::max(1, avail_h / static_cast<int>(stack_count));
                    const int y = area_y + (static_cast<int>(index - 1) * (stack_h + gaps.inner));
                    const int height = (index == total - 1) ? std::max(1, (area_y + area_h) - y) : stack_h;
                    cell = {.x = stack_x, .y = y, .width = stack_w, .height = height};
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
        if (m_active_fullscreen == window) {
            m_active_fullscreen = nullptr;
        }
        std::erase(m_focus_stack, window);
        std::erase(m_windows, window);
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
            tiled[i]->set_geometry(tile_box(usable, i, tiled.size(), layout_cfg));
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

        std::vector<Window*> tiled;
        for (Window* w : m_windows) {
            if (w->is_tiled()) {
                tiled.push_back(w);
            }
        }
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

    size_t Workspace::tiled_count() const noexcept {
        return static_cast<size_t>(std::ranges::count_if(m_windows, &Window::is_tiled));
    }

    wlr_box Workspace::next_tile_box() const noexcept {
        const auto& layout = m_output.compositor().config().layout();
        const size_t count = (layout.mode == Layout::Monocle) ? 0 : tiled_count();
        return tile_box(m_output.usable_area(), count, count + 1, layout);
    }

    bool Workspace::is_solitary() const noexcept {
        return m_output.compositor().config().layout().mode == Layout::Monocle || tiled_count() <= 1;
    }

    void ClosingWindow::apply_transform(double progress) noexcept {
        const double p = std::clamp(progress, 0.0, 1.0);
        const double scale = 0.90 + (0.10 * p);
        const float opacity = static_cast<float>(p);

        const int scaled_w = std::max(1, static_cast<int>(std::round(box.width * scale)));
        const int scaled_h = std::max(1, static_cast<int>(std::round(box.height * scale)));

        const int render_x = box.x + ((box.width - scaled_w) / 2);
        const int render_y = box.y + ((box.height - scaled_h) / 2);
        wlr_scene_node_set_position(&tree->node, render_x, render_y);

        for (auto& cb : buffers) {
            const int bw = std::max(1, static_cast<int>(std::round(cb.width * scale)));
            const int bh = std::max(1, static_cast<int>(std::round(cb.height * scale)));
            wlr_scene_buffer_set_dest_size(cb.buffer, bw, bh);
            wlr_scene_buffer_set_opacity(cb.buffer, cb.base_opacity * opacity);
            wlr_scene_node_set_position(&cb.buffer->node, static_cast<int>(std::round(cb.rel_x * scale)),
                                        static_cast<int>(std::round(cb.rel_y * scale)));
        }

        if (has_borders) {
            layout_border_rects(borders, scaled_w, scaled_h, border_width, border_color, opacity);
        }
    }

    void Workspace::add_closing_window(Window* window, double initial_progress) {
        if (!window || window->is_fullscreen() || initial_progress <= 0.01 || &m_output.active_workspace() != this) {
            return;
        }

        const auto& comp = m_output.compositor();
        if (comp.is_locked() || comp.is_workspace_swiping() || !m_output.is_enabled() ||
            m_output.animation().is_busy()) {
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
            closing->has_borders = true;
            const auto& border_cfg = comp.config().layout().border;
            closing->border_width = border_cfg.width;
            closing->border_color = window->is_activated() ? border_cfg.active_color : border_cfg.inactive_color;

            for (auto& border : closing->borders) {
                border = wlr_scene_rect_create(closing->tree, 0, 0, closing->border_color.data());
            }
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

        std::erase_if(m_closing_windows, [&](auto& cw) {
            const double dt = std::clamp(elapsed_seconds(cw->last_time, now), 0.0, 0.05);
            cw->last_time = now;
            cw->spring.advance(dt);

            if (cw->spring.is_settled(0.005, 0.05) || cw->spring.value() <= 0.005) {
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

}
