#pragma once

#include "frost/animation.hpp"
#include "frost/wlroots.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace frost {

    class Scene;
    class Window;
    class Output;
    enum class Direction : uint8_t;

    struct LayoutMetrics {
        Window* master = nullptr;
        size_t tiled_count = 0;
        int area_x = 0;
        int area_y = 0;
        int area_w = 1;
        int area_h = 1;
        int avail_w = 1;
        int avail_h = 1;
        int master_w = 1;
        int stack_x = 0;
        int stack_w = 1;
        int inner_gap = 0;
        int border_width = 0;
        float total_stack_weight = 0.0f;

        [[nodiscard]] constexpr float effective_stack_weight() const noexcept {
            return (total_stack_weight > 0.0f) ? total_stack_weight
                                               : static_cast<float>(tiled_count > 1 ? tiled_count - 1 : 0);
        }
    };

    struct TiledNeighbors {
        Window* master = nullptr;
        Window* first_stack = nullptr;
        Window* last = nullptr;
        Window* prev = nullptr;
        Window* next = nullptr;
        size_t count = 0;
        size_t index = 0;
        bool found = false;
    };

    class Workspace {
    public:
        Workspace(uint32_t id, Output& output, Scene& scene, wlr_ext_workspace_group_handle_v1* group);
        ~Workspace();

        Workspace(const Workspace&) = delete;
        Workspace& operator=(const Workspace&) = delete;

        [[nodiscard]] uint32_t id() const noexcept { return m_id; }
        [[nodiscard]] Output& output() const noexcept { return m_output; }
        [[nodiscard]] wlr_scene_tree* tree() const noexcept { return m_tree; }
        [[nodiscard]] std::span<Window* const> focus_stack() const noexcept { return m_focus_stack; }
        [[nodiscard]] std::span<Window* const> windows() const noexcept { return m_windows; }
        [[nodiscard]] Window* focused_window() const noexcept {
            return m_focus_stack.empty() ? nullptr : m_focus_stack.front();
        }

        void activate() noexcept;
        void deactivate() noexcept;
        void prepare_slide() noexcept;
        void snap_animations() noexcept;

        void promote(Window* window);
        void remove(Window* window) noexcept;
        Window* cycle();

        void arrange();
        void restack();
        void swap_windows(Window* a, Window* b);
        void swap_tiled_at(Window* window, double cursor_x, double cursor_y);
        [[nodiscard]] bool swap_window_in_direction(Direction direction);
        [[nodiscard]] Window* window_in_direction(Direction direction) const;
        [[nodiscard]] wlr_box next_tile_box() const noexcept;
        [[nodiscard]] bool is_solitary() const noexcept;
        [[nodiscard]] size_t tiled_count() const noexcept;

        [[nodiscard]] bool has_closing_animations() const noexcept { return !m_closing_windows.empty(); }
        void add_closing_window(Window* window, double initial_progress = 1.0);
        bool step_closing_animations(TimePoint now) noexcept;
        void snap_closing_animations() noexcept;

        [[nodiscard]] bool is_resizing() const noexcept;
        [[nodiscard]] uint32_t tiled_resize_edges(const Window* window, double cursor_x,
                                                  double cursor_y) const noexcept;
        [[nodiscard]] LayoutMetrics layout_metrics() const noexcept;
        [[nodiscard]] TiledNeighbors tiled_neighbors(const Window* target) const noexcept;
        [[nodiscard]] float master_factor() const noexcept { return m_master_factor; }
        void set_master_factor(float factor) noexcept { m_master_factor = factor; }

    private:
        [[nodiscard]] Window* top_stack_window(const Window* master, Window* fallback) const noexcept;

        uint32_t m_id = 1;
        Output& m_output;
        wlr_scene_tree* m_tree = nullptr;
        wlr_ext_workspace_handle_v1* m_handle = nullptr;
        Window* m_active_fullscreen = nullptr;
        std::vector<Window*> m_focus_stack;
        std::vector<Window*> m_windows;
        std::vector<std::unique_ptr<ClosingWindow>> m_closing_windows;
        float m_master_factor = 0.5f;
    };

}
