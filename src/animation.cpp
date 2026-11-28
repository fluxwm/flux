#include "frost/animation.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/output.hpp"
#include "frost/popup.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

#include <algorithm>
#include <cmath>

namespace frost {

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
        if (m_current) {
            wlr_scene_node_set_position(&m_current->tree()->node, current_x, 0);
            wlr_scene_node_set_enabled(&m_current->tree()->node, true);
        }
        if (m_outgoing && m_outgoing != m_current) {
            wlr_scene_node_set_position(&m_outgoing->tree()->node, current_x + m_outgoing_offset, 0);
            wlr_scene_node_set_enabled(&m_outgoing->tree()->node, true);
        }
        if (m_current) {
            if (m_current->output().compositor().outputs().size() > 1) {
                m_current->apply_slide_clip(current_x);
                if (m_outgoing && m_outgoing != m_current) {
                    m_outgoing->apply_slide_clip(current_x + m_outgoing_offset);
                }
            } else {
                m_current->clear_slide_clips();
                if (m_outgoing && m_outgoing != m_current) {
                    m_outgoing->clear_slide_clips();
                }
            }
            m_current->output().compositor().update_popup_positions();
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

        m_last_time = Clock::now();

        m_spring.reset(start_x, start_v);
        m_spring.set_target(0.0);
        apply_positions(static_cast<int>(std::round(start_x)));
    }

    void WorkspaceAnimation::begin_swipe(Output& output) noexcept {
        if (m_animating) {
            cancel();
        }
        output.active_workspace().snap_animations();
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
        uint32_t max_id = 9;
        for (const auto& ws : output.workspaces()) {
            max_id = std::max(max_id, ws->id());
        }
        const double min_progress = (cur_id < max_id) ? -1.0 : 0.0;
        const double max_progress = (cur_id > 1) ? 1.0 : 0.0;
        const double old_progress = m_swipe_progress;
        m_swipe_progress = std::clamp(m_swipe_progress + delta, min_progress, max_progress);
        const double actual_delta = m_swipe_progress - old_progress;

        const int span = std::max(1, output.layout_box().width);
        const int offset = static_cast<int>(std::round(m_swipe_progress * span));

        Workspace* target = nullptr;
        if (offset < 0 && cur_id < max_id) {
            target = &output.workspace(cur_id + 1);
            m_outgoing_offset = span;
        } else if (offset > 0 && cur_id > 1) {
            target = &output.workspace(cur_id - 1);
            m_outgoing_offset = -span;
        }

        if (m_outgoing && m_outgoing != target) {
            m_outgoing->deactivate();
        }
        if (target && m_outgoing != target) {
            target->restack();
        }
        m_outgoing = target;
        apply_positions(offset);

        const TimePoint now = Clock::now();
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

        if (!m_outgoing) {
            cancel();
            return nullptr;
        }
        m_swiping = false;

        const TimePoint now = Clock::now();
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

        if (!output.compositor().config().animations().enabled || m_spring.is_settled()) {
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

        const TimePoint now = Clock::now();
        const double dt = step_dt(m_last_time, now, 0.1, output.compositor().config().animations().slowdown);
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

    ClosingWindow::~ClosingWindow() {
        if (m_tree) {
            wlr_scene_node_destroy(&m_tree->node);
        }
    }

    std::unique_ptr<ClosingWindow> ClosingWindow::capture(wlr_scene_tree* parent, const Window& window,
                                                          const BorderConfig& border_cfg, double initial_progress) {
        if (!parent || !window.scene_tree()) {
            return nullptr;
        }
        const wlr_box box = window.geometry_box();
        if (box.width <= 0 || box.height <= 0) {
            return nullptr;
        }

        auto closing = std::make_unique<ClosingWindow>();
        closing->m_tree = wlr_scene_tree_create(parent);
        if (!closing->m_tree) {
            return nullptr;
        }

        int win_lx = 0;
        int win_ly = 0;
        wlr_scene_node_coords(&window.scene_tree()->node, &win_lx, &win_ly);

        closing->m_box = box;
        closing->m_is_floating = window.is_floating();

        struct CloneContext {
            wlr_scene_tree* tree;
            int win_lx;
            int win_ly;
            std::vector<Buffer>* buffers;
        } ctx{
            .tree = closing->m_tree,
            .win_lx = win_lx,
            .win_ly = win_ly,
            .buffers = &closing->m_buffers,
        };

        wlr_scene_node_for_each_buffer(
            &window.scene_tree()->node,
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

                c->buffers->push_back(Buffer{
                    .buffer = clone,
                    .rel_x = sx - c->win_lx,
                    .rel_y = sy - c->win_ly,
                    .width = orig_w,
                    .height = orig_h,
                    .base_opacity = src->opacity,
                });
            },
            &ctx);

        if (closing->m_buffers.empty()) {
            return nullptr;
        }

        if (window.has_visible_borders()) {
            closing->m_border_width = border_cfg.width;
            closing->m_border_color = window.is_activated() ? border_cfg.active_color : border_cfg.inactive_color;
            closing->m_border.init(closing->m_tree);
        }

        const double initial_p = std::clamp(initial_progress, 0.0, 1.0);
        closing->apply_transform(initial_p);
        closing->m_spring.reset(initial_p, 0.0);
        closing->m_spring.set_target(0.0);
        closing->m_last_time = Clock::now();
        return closing;
    }

    bool ClosingWindow::step(TimePoint now, double slowdown) noexcept {
        const double dt = step_dt(m_last_time, now, 0.05, slowdown);
        m_last_time = now;
        m_spring.advance(dt);

        if (m_spring.is_settled() || m_spring.value() <= 0.005) {
            return false;
        }

        apply_transform(m_spring.value());
        return true;
    }

    void ClosingWindow::apply_transform(double progress) noexcept {
        const double p = std::clamp(progress, 0.0, 1.0);
        const double scale = open_close_scale(p);
        const float opacity = static_cast<float>(p);

        const int scaled_w = scaled_extent(m_box.width, scale);
        const int scaled_h = scaled_extent(m_box.height, scale);

        const int render_x = m_box.x + center_offset(m_box.width, scaled_w);
        const int render_y = m_box.y + center_offset(m_box.height, scaled_h);
        wlr_scene_node_set_position(&m_tree->node, render_x, render_y);

        for (auto& cb : m_buffers) {
            wlr_scene_buffer_set_dest_size(cb.buffer, scaled_extent(cb.width, scale), scaled_extent(cb.height, scale));
            wlr_scene_buffer_set_opacity(cb.buffer, cb.base_opacity * opacity);
            wlr_scene_node_set_position(&cb.buffer->node, static_cast<int>(std::round(cb.rel_x * scale)),
                                        static_cast<int>(std::round(cb.rel_y * scale)));
        }

        m_border.update(scaled_w, scaled_h, m_border_width, m_border_color, opacity);
    }

    void WindowAnimation::start_open() noexcept {
        m_opening = true;
        m_open_spring.reset(0.0, 0.0);
        m_open_spring.set_target(1.0);
        m_last_time = Clock::now();
        apply_open_transform();
        if (auto* out = m_window.output(); out && out->raw()) {
            wlr_output_schedule_frame(out->raw());
        }
    }

    void WindowAnimation::move_scene_tree(int x, int y) noexcept {
        m_base_x = x;
        m_base_y = y;

        int render_x = x;
        int render_y = y;
        if (m_opening) {
            const wlr_box box = m_window.geometry_box();
            const double scale = open_close_scale(m_open_spring.value());
            const int scaled_w = scaled_extent(box.width, scale);
            const int scaled_h = scaled_extent(box.height, scale);
            render_x = x + center_offset(box.width, scaled_w);
            render_y = y + center_offset(box.height, scaled_h);
        }

        wlr_scene_tree* tree = m_window.m_scene_tree;
        if (tree->node.x == render_x && tree->node.y == render_y) {
            return;
        }
        wlr_scene_node_set_position(&tree->node, render_x, render_y);
        for (const auto& popup : m_window.m_compositor.popups()) {
            if (popup->root_tree() == tree) {
                popup->update_position();
            }
        }
    }

    bool WindowAnimation::can_animate() const noexcept {
        if (!m_window.m_compositor.config().animations().enabled) {
            return false;
        }
        if (!m_window.is_mapped() || !m_has_initial_position) {
            return false;
        }
        if (m_window.is_grabbed() || m_window.m_resizing || m_window.m_resize_edges != WLR_EDGE_NONE) {
            return false;
        }
        if (m_window.m_compositor.is_workspace_swiping() || m_window.m_compositor.is_locked()) {
            return false;
        }
        Workspace* ws = m_window.m_workspace;
        if (!ws || ws->is_resizing()) {
            return false;
        }
        Output& out = ws->output();
        if (!out.is_enabled() || out.animation().is_busy()) {
            return false;
        }
        return ws == &out.active_workspace();
    }

    void WindowAnimation::apply_position() noexcept {
        if (!can_animate()) {
            snap();
        } else {
            if (m_moving || m_base_x != m_target_x || m_base_y != m_target_y) {
                if (!m_moving) {
                    m_spring_x.reset(m_base_x, 0.0);
                    m_spring_y.reset(m_base_y, 0.0);
                    if (!m_opening) {
                        m_last_time = Clock::now();
                    }
                    m_moving = true;
                    if (auto* out = m_window.output(); out && out->raw()) {
                        wlr_output_schedule_frame(out->raw());
                    }
                }
                m_spring_x.set_target(m_target_x);
                m_spring_y.set_target(m_target_y);
                if (m_spring_x.is_settled() && m_spring_y.is_settled()) {
                    m_moving = false;
                }
            }
            if (!m_moving) {
                m_spring_x.reset(m_target_x, 0.0);
                m_spring_y.reset(m_target_y, 0.0);
                move_scene_tree(m_target_x, m_target_y);
            }
        }

        if (m_window.is_mapped()) {
            m_has_initial_position = true;
        }
    }

    bool WindowAnimation::step(TimePoint now) noexcept {
        if (!m_moving && !m_opening) {
            return false;
        }

        const double dt = step_dt(m_last_time, now, 0.05, m_window.m_compositor.config().animations().slowdown);
        m_last_time = now;

        if (m_moving) {
            m_spring_x.advance(dt);
            m_spring_y.advance(dt);

            if (m_spring_x.is_settled() && m_spring_y.is_settled()) {
                m_moving = false;
                m_spring_x.reset(m_target_x, 0.0);
                m_spring_y.reset(m_target_y, 0.0);
            }
        }

        m_base_x = m_moving ? static_cast<int>(std::round(m_spring_x.value())) : m_target_x;
        m_base_y = m_moving ? static_cast<int>(std::round(m_spring_y.value())) : m_target_y;

        if (m_opening) {
            m_open_spring.advance(dt);
            if (m_open_spring.is_settled()) {
                reset_open_transform();
            } else {
                apply_open_transform();
            }
        } else {
            move_scene_tree(m_base_x, m_base_y);
        }

        return m_moving || m_opening;
    }

    void WindowAnimation::snap() noexcept {
        m_moving = false;
        m_spring_x.reset(m_target_x, 0.0);
        m_spring_y.reset(m_target_y, 0.0);
        if (m_opening) {
            m_base_x = m_target_x;
            m_base_y = m_target_y;
            reset_open_transform();
            return;
        }
        move_scene_tree(m_target_x, m_target_y);
    }

    void WindowAnimation::apply_open_transform() noexcept {
        const wlr_box box = m_window.geometry_box();
        if (m_opening && (box.width <= 0 || box.height <= 0)) {
            return;
        }

        const double p = open_progress();
        const double scale = open_close_scale(p);
        const float opacity = static_cast<float>(p);

        struct TransformData {
            const wlr_surface* main_surface;
            double scale;
            float opacity;
            int fallback_w;
            int fallback_h;
        } data{
            .main_surface = m_window.surface(),
            .scale = scale,
            .opacity = opacity,
            .fallback_w = m_window.m_committed_width > 0 ? m_window.m_committed_width : box.width,
            .fallback_h = m_window.m_committed_height > 0 ? m_window.m_committed_height : box.height,
        };

        wlr_scene_node_for_each_buffer(
            &m_window.m_scene_tree->node,
            [](wlr_scene_buffer* buffer, int, int, void* user_data) {
                if (!buffer) {
                    return;
                }
                const auto* td = static_cast<const TransformData*>(user_data);
                int orig_w = 0;
                int orig_h = 0;
                float base_alpha = 1.0f;
                if (wlr_scene_surface* surf = wlr_scene_surface_try_from_buffer(buffer)) {
                    orig_w = surf->surface->current.width > 0
                                 ? surf->surface->current.width
                                 : (surf->surface == td->main_surface ? td->fallback_w : 0);
                    orig_h = surf->surface->current.height > 0
                                 ? surf->surface->current.height
                                 : (surf->surface == td->main_surface ? td->fallback_h : 0);
                    if (const auto* sub = wlr_subsurface_try_from_wlr_surface(surf->surface);
                        sub && buffer->node.parent) {
                        wlr_scene_node_set_position(&buffer->node.parent->node,
                                                    static_cast<int>(std::round(sub->current.x * td->scale)),
                                                    static_cast<int>(std::round(sub->current.y * td->scale)));
                    }
                    if (const auto* state = wlr_alpha_modifier_v1_get_surface_state(surf->surface)) {
                        base_alpha = static_cast<float>(state->multiplier);
                    }
                } else if (buffer->buffer) {
                    orig_w = buffer->buffer->width;
                    orig_h = buffer->buffer->height;
                }

                if (orig_w > 0 && orig_h > 0) {
                    wlr_scene_buffer_set_dest_size(buffer, scaled_extent(orig_w, td->scale),
                                                   scaled_extent(orig_h, td->scale));
                }
                wlr_scene_buffer_set_opacity(buffer, base_alpha * td->opacity);
            },
            &data);

        if (m_window.is_mapped()) {
            m_window.update_borders();
        }

        move_scene_tree(m_base_x, m_base_y);
    }

    void WindowAnimation::reset_open_transform() noexcept {
        m_opening = false;
        m_open_spring.reset(1.0, 0.0);
        apply_open_transform();
    }

}
