#pragma once

#include <cmath>
#include <ctime>

namespace flux {

    class Output;
    class Workspace;

    struct SpringConfig {
        double stiffness = 800.0;
        double damping_ratio = 1.0;
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

        [[nodiscard]] bool is_settled(double pos_epsilon = 0.5, double vel_epsilon = 1.0) const noexcept {
            return std::abs(m_value - m_target) < pos_epsilon && std::abs(m_velocity) < vel_epsilon;
        }

        void advance(double dt) noexcept;

    private:
        double m_value = 0.0;
        double m_target = 0.0;
        double m_velocity = 0.0;
        SpringConfig m_config;
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
        timespec m_last_time{};
        bool m_has_last_time = false;
    };

}
