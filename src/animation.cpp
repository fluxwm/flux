#include "flux/animation.hpp"
#include "flux/output.hpp"
#include "flux/window.hpp"
#include "flux/workspace.hpp"

#include <algorithm>
#include <cmath>

namespace flux {

    namespace {
        double elapsed_seconds(const timespec& from, const timespec& to) noexcept {
            return static_cast<double>(to.tv_sec - from.tv_sec) +
                   (static_cast<double>(to.tv_nsec - from.tv_nsec) * 1e-9);
        }

        void apply_workspace_offset(Workspace& ws, int offset_x, const wlr_box& output_box) noexcept {
            const int old_x = ws.tree()->node.x;
            const bool shrinking = !ws.tree()->node.enabled || std::abs(offset_x) >= std::abs(old_x);
            const wlr_box clip{
                .x = output_box.x - offset_x,
                .y = output_box.y,
                .width = output_box.width,
                .height = output_box.height,
            };

            if (shrinking) {
                ws.set_clip(&clip);
                wlr_scene_node_set_position(&ws.tree()->node, offset_x, 0);
            } else {
                wlr_scene_node_set_position(&ws.tree()->node, offset_x, 0);
                ws.set_clip(&clip);
            }
            for (Window* win : ws.windows()) {
                win->update_borders();
            }
            wlr_scene_node_set_enabled(&ws.tree()->node, true);
        }
    }

    void Spring::advance(double dt) noexcept {
        if (dt <= 0.0) {
            return;
        }
        if (is_settled()) {
            m_value = m_target;
            m_velocity = 0.0;
            return;
        }

        const double w0 = std::sqrt(m_config.stiffness);
        const double z = m_config.damping_ratio;
        const double x0 = m_value - m_target;
        const double v0 = m_velocity;

        if (std::abs(z - 1.0) < 1e-4) {
            const double c1 = x0;
            const double c2 = v0 + (w0 * x0);
            const double decay = std::exp(-w0 * dt);
            m_value = m_target + ((c1 + (c2 * dt)) * decay);
            m_velocity = (c2 - (w0 * (c1 + (c2 * dt)))) * decay;
        } else if (z < 1.0) {
            const double wd = w0 * std::sqrt(1.0 - (z * z));
            const double gamma = z * w0;
            const double decay = std::exp(-gamma * dt);
            const double a = x0;
            const double b = (v0 + (gamma * x0)) / wd;
            const double cos_wdt = std::cos(wd * dt);
            const double sin_wdt = std::sin(wd * dt);
            m_value = m_target + (decay * ((a * cos_wdt) + (b * sin_wdt)));
            m_velocity = decay * ((((b * wd) - (a * gamma)) * cos_wdt) - (((a * wd) + (b * gamma)) * sin_wdt));
        } else {
            const double wd = w0 * std::sqrt((z * z) - 1.0);
            const double gamma = z * w0;
            const double l1 = -gamma + wd;
            const double l2 = -gamma - wd;
            const double c2 = (v0 - (l1 * x0)) / (l2 - l1);
            const double c1 = x0 - c2;
            const double e1 = std::exp(l1 * dt);
            const double e2 = std::exp(l2 * dt);
            m_value = m_target + (c1 * e1) + (c2 * e2);
            m_velocity = (c1 * l1 * e1) + (c2 * l2 * e2);
        }

        if (is_settled()) {
            m_value = m_target;
            m_velocity = 0.0;
        }
    }

    void WorkspaceAnimation::apply_positions(int current_x) noexcept {
        if (!m_current && !m_outgoing) {
            return;
        }

        const Output& output = m_current ? m_current->output() : m_outgoing->output();
        const wlr_box output_box = output.layout_box();

        if (m_current) {
            apply_workspace_offset(*m_current, current_x, output_box);
        }
        if (m_outgoing && m_outgoing != m_current) {
            apply_workspace_offset(*m_outgoing, current_x + m_outgoing_offset, output_box);
        }
    }

    void WorkspaceAnimation::start(Workspace& from, Workspace& to, int span) noexcept {
        if (&from == &to || span <= 0) {
            cancel();
            return;
        }

        double start_x = 0.0;
        double start_v = 0.0;

        if (m_animating && m_current == &from && m_outgoing == &to) {
            start_x = m_spring.value() + m_outgoing_offset;
            start_v = m_spring.velocity();
            m_outgoing_offset = -m_outgoing_offset;
        } else {
            if (m_outgoing && m_outgoing != &from && m_outgoing != &to) {
                m_outgoing->deactivate();
            }
            if (m_current && m_current != &from && m_current != &to) {
                m_current->deactivate();
            }
            const int dir = (to.id() > from.id()) ? 1 : -1;
            start_x = static_cast<double>(dir * span);
            m_outgoing_offset = -dir * span;
        }

        m_current = &to;
        m_outgoing = &from;
        m_swiping = false;
        m_animating = true;

        from.prepare_slide();
        to.activate();

        clock_gettime(CLOCK_MONOTONIC, &m_last_time);

        m_spring.reset(start_x, start_v);
        m_spring.set_target(0.0);
        apply_positions(static_cast<int>(std::round(start_x)));
    }

    void WorkspaceAnimation::begin_swipe(Output& output) noexcept {
        if (m_animating) {
            cancel();
        }
        m_swiping = true;
        m_swipe_progress = 0.0;
        m_swipe_velocity = 0.0;
        m_has_last_time = false;
        m_current = &output.active_workspace();
        m_outgoing = nullptr;
        m_outgoing_offset = 0;
    }

    void WorkspaceAnimation::update_swipe(Output& output, double dx, bool natural_scroll) noexcept {
        if (!m_swiping || !m_current) {
            return;
        }

        constexpr double swipe_distance = 350.0;
        const double delta = (natural_scroll ? dx : -dx) / swipe_distance;
        const uint32_t cur_id = m_current->id();
        const double min_progress = (cur_id < 9) ? -1.0 : 0.0;
        const double max_progress = (cur_id > 1) ? 1.0 : 0.0;
        const double old_progress = m_swipe_progress;
        m_swipe_progress = std::clamp(m_swipe_progress + delta, min_progress, max_progress);
        const double actual_delta = m_swipe_progress - old_progress;

        const int span = std::max(1, output.layout_box().width);
        const int offset = static_cast<int>(std::round(m_swipe_progress * span));

        Workspace* target = nullptr;
        if (offset < 0 && cur_id < 9) {
            target = &output.workspace(cur_id + 1);
            m_outgoing_offset = span;
        } else if (offset > 0 && cur_id > 1) {
            target = &output.workspace(cur_id - 1);
            m_outgoing_offset = -span;
        }

        if (m_outgoing && m_outgoing != target) {
            m_outgoing->deactivate();
        }
        m_outgoing = target;
        apply_positions(offset);

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (m_has_last_time) {
            const double dt = elapsed_seconds(m_last_time, now);
            if (dt > 1e-4) {
                const double v_inst = std::clamp((actual_delta * span) / dt, -10000.0, 10000.0);
                m_swipe_velocity = (0.5 * m_swipe_velocity) + (0.5 * v_inst);
            }
        }
        m_last_time = now;
        m_has_last_time = true;
    }

    Workspace* WorkspaceAnimation::end_swipe(Output& output) noexcept {
        if (!m_swiping || !m_current) {
            return nullptr;
        }

        m_swiping = false;
        if (!m_outgoing) {
            cancel();
            return nullptr;
        }

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (m_has_last_time && elapsed_seconds(m_last_time, now) > 0.08) {
            m_swipe_velocity = 0.0;
        }

        constexpr double commit_threshold = 0.25;
        constexpr double fling_velocity_threshold = 400.0;

        const int dir = (m_outgoing->id() > m_current->id()) ? -1 : 1;
        const bool commit = (std::abs(m_swipe_velocity) > fling_velocity_threshold)
                                ? ((dir == -1 && m_swipe_velocity < 0.0) || (dir == 1 && m_swipe_velocity > 0.0))
                                : (std::abs(m_swipe_progress) >= commit_threshold);

        const int span = std::max(1, output.layout_box().width);
        const double cur_offset = m_swipe_progress * span;
        double initial_x = cur_offset;
        Workspace* activated_target = nullptr;

        m_animating = true;
        if (commit) {
            Workspace* cur = m_current;
            Workspace* target = m_outgoing;
            cur->prepare_slide();
            output.set_active_workspace(target);
            target->activate();
            m_current = target;
            m_outgoing = cur;
            m_outgoing_offset = dir * span;
            initial_x = cur_offset - m_outgoing_offset;
            activated_target = target;
        }

        const double max_speed = std::sqrt(m_spring.config().stiffness) * std::abs(initial_x);
        const double clamped_v = (initial_x > 0.0) ? std::clamp(m_swipe_velocity, -max_speed, 0.0)
                                                   : std::clamp(m_swipe_velocity, 0.0, max_speed);

        m_spring.reset(initial_x, clamped_v);
        m_spring.set_target(0.0);
        m_last_time = now;

        if (m_spring.is_settled()) {
            cancel();
        } else {
            apply_positions(static_cast<int>(std::round(initial_x)));
            wlr_output_schedule_frame(output.raw());
        }
        return activated_target;
    }

    void WorkspaceAnimation::step(Output& output) noexcept {
        if (!m_animating) {
            return;
        }

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double dt = std::clamp(elapsed_seconds(m_last_time, now), 0.0, 0.1);
        m_last_time = now;

        m_spring.advance(dt);

        if (m_spring.is_settled()) {
            cancel();
        } else {
            apply_positions(static_cast<int>(std::round(m_spring.value())));
            wlr_output_schedule_frame(output.raw());
        }
    }

    void WorkspaceAnimation::cancel() noexcept {
        if (!m_animating && !m_swiping) {
            return;
        }
        m_animating = false;
        m_swiping = false;
        if (m_outgoing && m_outgoing != m_current) {
            m_outgoing->deactivate();
        }
        if (m_current) {
            m_current->activate();
        }
        m_current = nullptr;
        m_outgoing = nullptr;
        m_outgoing_offset = 0;
    }

}
