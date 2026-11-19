#pragma once

#include "flux/animation.hpp"
#include "flux/wlroots.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace flux {

    class Scene;
    class Window;
    class Output;
    enum class Direction : uint8_t;

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
        [[nodiscard]] Window* window_in_direction(Direction direction) const;
        [[nodiscard]] wlr_box next_tile_box() const noexcept;
        [[nodiscard]] bool is_solitary() const noexcept;
        [[nodiscard]] size_t tiled_count() const noexcept;

        [[nodiscard]] bool has_closing_animations() const noexcept { return !m_closing_windows.empty(); }
        void add_closing_window(Window* window, double initial_progress = 1.0);
        bool step_closing_animations(const timespec& now) noexcept;
        void snap_closing_animations() noexcept;

        [[nodiscard]] bool is_resizing() const noexcept { return m_is_resizing; }
        [[nodiscard]] uint32_t tiled_resize_edges(const Window* window, double cursor_x,
                                                  double cursor_y) const noexcept;
        void start_tiled_resize(Window* window, uint32_t edges);
        void update_tiled_resize(int dx, int dy);
        void end_tiled_resize() noexcept;

    private:
        [[nodiscard]] std::vector<Window*> tiled_windows() const;

        uint32_t m_id = 1;
        Output& m_output;
        wlr_scene_tree* m_tree = nullptr;
        wlr_ext_workspace_handle_v1* m_handle = nullptr;
        Window* m_active_fullscreen = nullptr;
        std::vector<Window*> m_focus_stack;
        std::vector<Window*> m_windows;
        std::vector<std::unique_ptr<ClosingWindow>> m_closing_windows;
        float m_master_factor = 0.5f;
        bool m_is_resizing = false;
        uint32_t m_grab_edges = WLR_EDGE_NONE;
        float m_grab_master_factor = 0.5f;
        Window* m_grab_top_win = nullptr;
        Window* m_grab_bot_win = nullptr;
        float m_grab_top_weight = 1.0f;
        float m_grab_bot_weight = 1.0f;
    };

}
