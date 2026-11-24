#pragma once

#include "frost/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

namespace frost {

    class Output;
    class Workspace;
    class Window;

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] inline double elapsed_seconds(TimePoint from, TimePoint to) noexcept {
        return std::chrono::duration<double>(to - from).count();
    }

    [[nodiscard]] inline double step_dt(TimePoint from, TimePoint to, double max_dt, double slowdown) noexcept {
        return std::clamp(elapsed_seconds(from, to), 0.0, max_dt) / std::max(0.001, slowdown);
    }

    [[nodiscard]] inline double open_close_scale(double progress) noexcept {
        return 0.90 + (0.10 * std::clamp(progress, 0.0, 1.0));
    }

    [[nodiscard]] inline int scaled_extent(int extent, double scale) noexcept {
        return std::max(1, static_cast<int>(std::round(extent * scale)));
    }

    [[nodiscard]] inline int center_offset(int total, int scaled) noexcept {
        return (total - scaled) / 2;
    }

    struct SpringConfig {
        double stiffness = 800.0;
        double damping_ratio = 1.0;
        double pos_epsilon = 0.5;
        double vel_epsilon = 1.0;
    };

    class Spring {
    public:
        explicit Spring(double initial = 0.0, SpringConfig config = {}) noexcept
            : m_value(initial), m_target(initial), m_config(config) {}

        void reset(double value, double velocity = 0.0) noexcept {
            m_value = value;
            m_target = value;
            m_velocity = velocity;
        }

        void set_target(double target) noexcept { m_target = target; }

        [[nodiscard]] double value() const noexcept { return m_value; }
        [[nodiscard]] double velocity() const noexcept { return m_velocity; }
        [[nodiscard]] const SpringConfig& config() const noexcept { return m_config; }

        [[nodiscard]] bool is_settled() const noexcept {
            return std::abs(m_value - m_target) < m_config.pos_epsilon && std::abs(m_velocity) < m_config.vel_epsilon;
        }

        void advance(double dt) noexcept;

    private:
        double m_value = 0.0;
        double m_target = 0.0;
        double m_velocity = 0.0;
        SpringConfig m_config;
    };

    struct ClosingBuffer {
        wlr_scene_buffer* buffer = nullptr;
        int rel_x = 0;
        int rel_y = 0;
        int width = 0;
        int height = 0;
        float base_opacity = 1.0f;
    };

    struct ClosingWindow {
        wlr_scene_tree* tree = nullptr;
        std::vector<ClosingBuffer> buffers;
        Border border;
        std::array<float, 4> border_color{0.0f, 0.0f, 0.0f, 0.0f};
        wlr_box box{};
        int border_width = 0;
        bool is_floating = false;
        Spring spring{1.0, {.stiffness = 1000.0, .damping_ratio = 1.0, .pos_epsilon = 0.005, .vel_epsilon = 0.05}};
        TimePoint last_time{};

        ClosingWindow() = default;
        ~ClosingWindow() {
            if (tree) {
                wlr_scene_node_destroy(&tree->node);
            }
        }

        ClosingWindow(const ClosingWindow&) = delete;
        ClosingWindow& operator=(const ClosingWindow&) = delete;
        ClosingWindow(ClosingWindow&&) = delete;
        ClosingWindow& operator=(ClosingWindow&&) = delete;

        void apply_transform(double progress) noexcept;
    };

    class WorkspaceAnimation {
    public:
        WorkspaceAnimation() = default;

        void start(Workspace& from, Workspace& to, int span) noexcept;
        void begin_swipe(Output& output) noexcept;
        void update_swipe(Output& output, double dx, bool natural_scroll) noexcept;
        Workspace* end_swipe(Output& output) noexcept;
        void step(Output& output) noexcept;
        void cancel() noexcept;

        [[nodiscard]] bool is_active() const noexcept { return m_animating; }
        [[nodiscard]] bool is_swiping() const noexcept { return m_swiping; }
        [[nodiscard]] bool is_busy() const noexcept { return m_animating || m_swiping; }

    private:
        void apply_positions(int current_x) noexcept;

        Spring m_spring{};
        Workspace* m_current = nullptr;
        Workspace* m_outgoing = nullptr;
        int m_outgoing_offset = 0;
        bool m_animating = false;
        bool m_swiping = false;
        double m_swipe_progress = 0.0;
        double m_swipe_velocity = 0.0;
        TimePoint m_last_time{};
        bool m_has_last_time = false;
    };

    class WindowAnimation {
    public:
        explicit WindowAnimation(Window& window) noexcept : m_window(window) {}

        [[nodiscard]] int base_x() const noexcept { return m_base_x; }
        [[nodiscard]] int base_y() const noexcept { return m_base_y; }
        [[nodiscard]] int target_x() const noexcept { return m_target_x; }
        [[nodiscard]] int target_y() const noexcept { return m_target_y; }
        [[nodiscard]] bool is_moving() const noexcept { return m_moving; }
        [[nodiscard]] bool is_opening() const noexcept { return m_opening; }
        [[nodiscard]] bool is_active() const noexcept { return m_moving || m_opening; }
        [[nodiscard]] double open_progress() const noexcept {
            return m_opening ? std::clamp(m_open_spring.value(), 0.0, 1.0) : 1.0;
        }
        [[nodiscard]] bool has_initial_position() const noexcept { return m_has_initial_position; }

        void set_target(int x, int y) noexcept {
            m_target_x = x;
            m_target_y = y;
        }
        void set_has_initial_position(bool has) noexcept { m_has_initial_position = has; }

        void start_open() noexcept;
        void apply_open_transform() noexcept;
        void reset_open_transform() noexcept;

        void apply_position() noexcept;
        bool step(TimePoint now) noexcept;
        void snap() noexcept;

        [[nodiscard]] bool can_animate() const noexcept;
        void move_scene_tree(int x, int y) noexcept;

    private:
        Window& m_window;
        Spring m_spring_x{};
        Spring m_spring_y{};
        Spring m_open_spring{1.0,
                             {.stiffness = 900.0, .damping_ratio = 1.0, .pos_epsilon = 0.005, .vel_epsilon = 0.05}};
        TimePoint m_last_time{};
        int m_base_x = 0;
        int m_base_y = 0;
        int m_target_x = 0;
        int m_target_y = 0;
        bool m_moving = false;
        bool m_opening = false;
        bool m_has_initial_position = false;
    };

}
