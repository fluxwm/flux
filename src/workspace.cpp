#include "flux/workspace.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/output.hpp"
#include "flux/scene.hpp"
#include "flux/window.hpp"

#include <algorithm>
#include <ranges>
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
        wlr_ext_workspace_handle_v1_destroy(m_handle);
        if (m_tree) {
            wlr_scene_node_destroy(&m_tree->node);
        }
    }

    void Workspace::set_clip(const wlr_box* clip) noexcept {
        wlr_scene_node* node = nullptr;
        wl_list_for_each(node, &m_tree->children, link) {
            if (const auto* win = static_cast<const Window*>(node->data); win && win->surface()) {
                wlr_scene_subsurface_tree_set_clip(&m_tree->node, clip);
                return;
            }
        }
    }

    void Workspace::activate() noexcept {
        wlr_ext_workspace_handle_v1_set_active(m_handle, true);
        if (m_output.animation().is_busy()) {
            return;
        }
        wlr_scene_node_set_position(&m_tree->node, 0, 0);
        set_clip(nullptr);
        wlr_scene_node_set_enabled(&m_tree->node, true);
        for (Window* w : m_windows) {
            w->update_borders();
        }
    }

    void Workspace::deactivate() noexcept {
        prepare_slide();
        wlr_scene_node_set_enabled(&m_tree->node, false);
        wlr_scene_node_set_position(&m_tree->node, 0, 0);
        set_clip(nullptr);
        for (Window* w : m_windows) {
            w->update_borders();
        }
    }

    void Workspace::prepare_slide() noexcept {
        for (Window* w : m_focus_stack) {
            if (w->is_fullscreen()) {
                w->reparent_to_workspace();
            }
        }
        wlr_ext_workspace_handle_v1_set_active(m_handle, false);
    }

    void Workspace::promote(Window* window) {
        if (!window) {
            return;
        }
        if (!std::ranges::contains(m_windows, window)) {
            m_windows.push_back(window);
        }
        if (focused_window() == window) {
            return;
        }
        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);
    }

    void Workspace::remove(Window* window) noexcept {
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
                if (!w->is_placed()) {
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

        if (m_output.animation().is_busy()) {
            const wlr_box clip{
                .x = full.x - m_tree->node.x,
                .y = full.y,
                .width = full.width,
                .height = full.height,
            };
            set_clip(&clip);
        }
    }

    void Workspace::restack() {
        const bool is_busy_output = m_output.animation().is_busy();
        Window* top_fullscreen = nullptr;
        Window* top_non_floating = nullptr;
        for (Window* w : m_focus_stack) {
            if (w->is_fullscreen() && !top_fullscreen) {
                top_fullscreen = w;
            }
            if (!w->is_floating() && !top_non_floating) {
                top_non_floating = w;
            }
        }

        for (Window* w : m_focus_stack) {
            if (w->is_fullscreen()) {
                if (w == top_fullscreen && !is_busy_output) {
                    wlr_scene_node_reparent(&w->scene_tree()->node, m_output.compositor().scene().fullscreen());
                } else {
                    w->reparent_to_workspace();
                }
            }
            if (!w->is_floating()) {
                if (w == top_non_floating) {
                    w->raise_to_top();
                } else if (!w->is_tiled()) {
                    w->lower_to_bottom();
                }
            }
        }

        for (Window* w : std::views::reverse(m_focus_stack)) {
            if (w->is_floating()) {
                w->raise_to_top();
            }
        }
    }

    Window* Workspace::window_in_direction(Direction direction) const {
        Window* focused = focused_window();
        if (!focused || !focused->is_tiled()) {
            return nullptr;
        }

        std::vector<Window*> tiled;
        for (Window* w : m_windows) {
            if (w->is_tiled()) {
                tiled.push_back(w);
            }
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

        if (m_output.compositor().config().layout().mode == Layout::Monocle) {
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
                case Direction::Right: {
                    auto stack_it = std::ranges::find_if(
                        m_focus_stack, [focused](const Window* w) { return w != focused && w->is_tiled(); });
                    return (stack_it != m_focus_stack.end()) ? *stack_it : tiled[1];
                }
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

}
