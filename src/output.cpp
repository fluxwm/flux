#include "frost/output.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/layer.hpp"
#include "frost/log.hpp"
#include "frost/scene.hpp"
#include "frost/util.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <limits>
#include <utility>

namespace frost {

    namespace {

        [[nodiscard]] wlr_output_mode* find_matching_mode(wlr_output* output, const OutputMode& req) noexcept {
            wlr_output_mode* best = nullptr;
            int32_t best_diff = std::numeric_limits<int32_t>::max();
            wlr_output_mode* mode = nullptr;
            wl_list_for_each(mode, &output->modes, link) {
                if (mode->width != req.width || mode->height != req.height) {
                    continue;
                }
                if (req.refresh_mhz) {
                    const int32_t diff = std::abs(mode->refresh - *req.refresh_mhz);
                    if (diff <= 1000 && diff < best_diff) {
                        best_diff = diff;
                        best = mode;
                    }
                } else if (!best || mode->refresh > best->refresh) {
                    best = mode;
                }
            }
            return best;
        }

    }

    Output::Output(Compositor& compositor, wlr_output* wlr_output)
        : m_compositor(compositor), m_wlr_output(wlr_output) {
        wlr_output_init_render(m_wlr_output, m_compositor.allocator(), m_compositor.renderer());

        apply_config(m_compositor.config().output_config(m_wlr_output), true);

        m_listeners.listen(&m_wlr_output->events.frame, this, &Output::on_frame);
        m_listeners.listen(&m_wlr_output->events.request_state, this, &Output::on_request_state);
        m_listeners.listen(&m_wlr_output->events.destroy, this, &Output::on_destroy);

        m_scene_output = m_compositor.scene().create_output(m_wlr_output);

        if (wlr_output_is_wl(m_wlr_output)) {
            wlr_wl_output_set_title(m_wlr_output, "Frost");
            wlr_wl_output_set_app_id(m_wlr_output, "frost");
        } else if (wlr_output_is_x11(m_wlr_output)) {
            wlr_x11_output_set_title(m_wlr_output, "Frost");
        }

        m_workspace_group = wlr_ext_workspace_group_handle_v1_create(m_compositor.workspace_manager(), 0);
        wlr_ext_workspace_group_handle_v1_output_enter(m_workspace_group, m_wlr_output);

        for (uint32_t i = 1; i <= 9; ++i) {
            m_workspaces.push_back(std::make_unique<Workspace>(i, *this, m_compositor.scene(), m_workspace_group));
        }
        m_active_workspace = m_workspaces.front().get();
        m_active_workspace->activate();
    }

    Output::~Output() {
        destroy_lock_surface();
        m_listeners.clear();
        if (m_scene_output) {
            wlr_scene_output_destroy(m_scene_output);
        }
        if (m_wlr_output && m_compositor.output_layout()) {
            wlr_output_layout_remove(m_compositor.output_layout(), m_wlr_output);
        }
        m_animation.cancel();
        destroy_layers();
        m_workspaces.clear();
        if (m_workspace_group) {
            wlr_ext_workspace_group_handle_v1_destroy(m_workspace_group);
        }
    }

    Workspace& Output::workspace(uint32_t id) {
        auto it = std::ranges::find_if(m_workspaces, [id](const auto& ws) { return ws->id() == id; });
        if (it != m_workspaces.end()) {
            return **it;
        }
        m_workspaces.push_back(std::make_unique<Workspace>(id, *this, m_compositor.scene(), m_workspace_group));
        return *m_workspaces.back();
    }

    void Output::on_frame() {
        if (!m_scene_output) {
            return;
        }

        const bool was_animating = m_animation.is_active();
        m_animation.step(*this);
        if (was_animating && !m_animation.is_active()) {
            m_compositor.on_animation_finished();
        }

        bool any_window_animating = false;
        bool had_animating_window = false;
        const TimePoint now = Clock::now();

        if (!m_animation.is_busy() && m_active_workspace) {
            for (Window* w : m_active_workspace->windows()) {
                if (w->is_animating()) {
                    had_animating_window = true;
                    if (w->step_animation(now)) {
                        any_window_animating = true;
                    }
                }
            }
            if (m_active_workspace->has_closing_animations()) {
                had_animating_window = true;
                if (m_active_workspace->step_closing_animations(now)) {
                    any_window_animating = true;
                }
            }
        }

        if (any_window_animating) {
            wlr_output_schedule_frame(m_wlr_output);
        } else if (had_animating_window) {
            m_compositor.update_cursor_focus();
        }

        if (wlr_scene_output_needs_frame(m_scene_output)) {
            wlr_output_state state;
            wlr_output_state_init(&state);
            if (wlr_scene_output_build_state(m_scene_output, &state, nullptr)) {
                if ((state.committed & WLR_OUTPUT_STATE_BUFFER) && m_compositor.allows_tearing(this)) {
                    state.tearing_page_flip = true;
                    if (!wlr_output_test_state(m_wlr_output, &state)) {
                        state.tearing_page_flip = false;
                    }
                }
                if (!wlr_output_commit_state(m_wlr_output, &state) && state.tearing_page_flip) {
                    state.tearing_page_flip = false;
                    wlr_output_commit_state(m_wlr_output, &state);
                }
            }
            wlr_output_state_finish(&state);
        }

        timespec frame_time{};
        clock_gettime(CLOCK_MONOTONIC, &frame_time);
        wlr_scene_output_send_frame_done(m_scene_output, &frame_time);
    }

    bool Output::apply_config(const OutputConfig* cfg, bool initial) {
        const bool prev_config_disabled = m_config_disabled;
        const bool prev_dpms_off = m_dpms_off;
        m_config_disabled = (cfg && !cfg->enabled);
        if (m_config_disabled) {
            m_dpms_off = false;
        }
        const bool want_enabled = !m_config_disabled && !m_dpms_off;
        const float target_scale = cfg ? cfg->scale : 1.0f;
        const wl_output_transform target_transform = cfg ? cfg->transform : WL_OUTPUT_TRANSFORM_NORMAL;

        wlr_output_mode* target_mode = nullptr;
        bool use_custom_mode = false;
        int32_t custom_w = 0;
        int32_t custom_h = 0;
        int32_t custom_refresh = 0;

        if (want_enabled) {
            if (cfg && cfg->mode) {
                if (!wl_list_empty(&m_wlr_output->modes)) {
                    target_mode = find_matching_mode(m_wlr_output, *cfg->mode);
                    if (!target_mode) {
                        if (cfg->mode->refresh_mhz) {
                            log_error(
                                "mode {}x{}@{:.3f}Hz not available on output '{}', falling back to preferred mode",
                                cfg->mode->width, cfg->mode->height,
                                static_cast<double>(*cfg->mode->refresh_mhz) / 1000.0, m_wlr_output->name);
                        } else {
                            log_error("mode {}x{} not available on output '{}', falling back to preferred mode",
                                      cfg->mode->width, cfg->mode->height, m_wlr_output->name);
                        }
                        target_mode = wlr_output_preferred_mode(m_wlr_output);
                    }
                } else {
                    use_custom_mode = true;
                    custom_w = cfg->mode->width;
                    custom_h = cfg->mode->height;
                    custom_refresh = cfg->mode->refresh_mhz.value_or(0);
                }
            } else {
                target_mode = wlr_output_preferred_mode(m_wlr_output);
            }
        }

        if (!initial) {
            const bool enable_changed = (m_wlr_output->enabled != want_enabled);
            if (!want_enabled) {
                if (!enable_changed) {
                    if (m_config_disabled) {
                        m_layout_box = {};
                    }
                    return true;
                }
            } else {
                const bool scale_changed = std::abs(m_wlr_output->scale - target_scale) > 1e-4f;
                const bool transform_changed = m_wlr_output->transform != target_transform;
                const bool refresh_changed = custom_refresh > 0 && m_wlr_output->refresh != custom_refresh;
                const bool mode_changed =
                    use_custom_mode
                        ? (m_wlr_output->width != custom_w || m_wlr_output->height != custom_h || refresh_changed)
                        : (target_mode != nullptr && m_wlr_output->current_mode != target_mode);
                if (!enable_changed && !scale_changed && !transform_changed && !mode_changed) {
                    return true;
                }
            }
        }

        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, want_enabled);
        if (want_enabled) {
            if (use_custom_mode) {
                wlr_output_state_set_custom_mode(&state, custom_w, custom_h, custom_refresh);
            } else if (target_mode) {
                wlr_output_state_set_mode(&state, target_mode);
            }
            wlr_output_state_set_scale(&state, target_scale);
            wlr_output_state_set_transform(&state, target_transform);
        }

        const bool committed = wlr_output_commit_state(m_wlr_output, &state);
        wlr_output_state_finish(&state);

        if (!committed) {
            log_error("failed to commit configuration for output '{}'", m_wlr_output->name);
            if (initial && want_enabled) {
                wlr_output_state fallback;
                wlr_output_state_init(&fallback);
                wlr_output_state_set_enabled(&fallback, true);
                if (wlr_output_mode* pref = wlr_output_preferred_mode(m_wlr_output)) {
                    wlr_output_state_set_mode(&fallback, pref);
                }
                wlr_output_state_set_scale(&fallback, 1.0f);
                wlr_output_state_set_transform(&fallback, WL_OUTPUT_TRANSFORM_NORMAL);
                const bool fallback_committed = wlr_output_commit_state(m_wlr_output, &fallback);
                wlr_output_state_finish(&fallback);
                return fallback_committed;
            }
            if (!initial) {
                m_config_disabled = prev_config_disabled;
                m_dpms_off = prev_dpms_off;
            }
            return false;
        }
        if (m_config_disabled) {
            m_layout_box = {};
        }
        return true;
    }

    void Output::on_request_state(const wlr_output_event_request_state* event) {
        if (m_config_disabled) {
            return;
        }
        const bool was_enabled = is_enabled();
        if (wlr_output_commit_state(m_wlr_output, event->state)) {
            if (was_enabled != is_enabled()) {
                if (is_enabled()) {
                    m_dpms_off = false;
                }
                m_compositor.on_output_enabled_changed(this);
                return;
            }
            if (is_enabled()) {
                arrange_layers();
            }
            m_compositor.sync_session_lock();
        }
    }

    void Output::on_destroy() {
        m_compositor.remove_output(this);
    }

    bool Output::is_nested() const noexcept {
        return wlr_output_is_wl(m_wlr_output) || wlr_output_is_x11(m_wlr_output);
    }

    wlr_box Output::layout_box() const noexcept {
        wlr_box box{};
        wlr_output_layout_get_box(m_compositor.output_layout(), m_wlr_output, &box);
        return box;
    }

    Output* Output::adjacent_output(Direction direction) const noexcept {
        const wlr_box cur_box = layout_box();
        const double ref_lx = cur_box.x + (static_cast<double>(cur_box.width) / 2.0);
        const double ref_ly = cur_box.y + (static_cast<double>(cur_box.height) / 2.0);

        wlr_output* adj_wlr = wlr_output_layout_adjacent_output(
            m_compositor.output_layout(), static_cast<wlr_direction>(direction), m_wlr_output, ref_lx, ref_ly);
        Output* adj_out = m_compositor.output_from_wlr(adj_wlr);
        return (adj_out && adj_out->is_enabled()) ? adj_out : nullptr;
    }

    void Output::add_layer(wlr_layer_surface_v1* layer_surface) {
        m_layers.push_back(std::make_unique<Layer>(*this, layer_surface));
    }

    void Output::remove_layer(Layer* layer) {
        if (!layer) {
            return;
        }

        const bool was_initialized = layer->is_initialized();
        erase_unique_ptr(m_layers, layer);

        if (was_initialized) {
            arrange_layers();
            m_compositor.refocus();
        }
    }

    void Output::destroy_layers() {
        for (auto& layer : std::exchange(m_layers, {})) {
            wlr_layer_surface_v1* raw = layer->raw();
            layer.reset();
            wlr_layer_surface_v1_destroy(raw);
        }
    }

    void Output::arrange_layers() {
        const wlr_box output_box = layout_box();
        if (m_layout_box.width > 0 && m_layout_box.height > 0 &&
            (m_layout_box.x != output_box.x || m_layout_box.y != output_box.y ||
             m_layout_box.width != output_box.width || m_layout_box.height != output_box.height)) {
            const int dx = output_box.x - m_layout_box.x;
            const int dy = output_box.y - m_layout_box.y;
            for (const auto& ws : m_workspaces) {
                for (Window* w : ws->windows()) {
                    if (!w->is_floating() || !w->is_placed()) {
                        continue;
                    }
                    wlr_box wbox = w->geometry_box();
                    if (dx != 0 || dy != 0) {
                        wbox.x += dx;
                        wbox.y += dy;
                        w->set_position(wbox.x, wbox.y);
                        w->snap_animation();
                    }
                    wlr_box intersection{};
                    if (output_box.width > 0 && output_box.height > 0 &&
                        !wlr_box_intersection(&intersection, &wbox, &output_box)) {
                        w->set_placed(false);
                    }
                }
            }
        }
        m_layout_box = output_box;

        wlr_box full_area = output_box;
        wlr_box usable_area = output_box;

        constexpr std::array layers = {
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP,
            ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
            ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        };

        auto configure = [&](bool exclusive) {
            for (auto layer : layers) {
                for (const auto& ls : m_layers) {
                    if (ls->scene_layer() && ls->is_initialized() && ls->layer() == layer &&
                        (ls->exclusive_zone() > 0) == exclusive) {
                        wlr_scene_layer_surface_v1_configure(ls->scene_layer(), &full_area, &usable_area);
                    }
                }
            }
        };

        configure(true);
        set_usable_area(usable_area);
        configure(false);

        m_compositor.update_popup_positions();

        for (const auto& ws : m_workspaces) {
            ws->arrange();
        }

        m_compositor.update_cursor_focus();
    }

    void Output::set_lock_surface(wlr_session_lock_surface_v1* lock_surface) {
        m_lock_surface = lock_surface;
        m_scene_lock_surface = wlr_scene_tree_create(m_compositor.scene().lock_surfaces());
        if (m_scene_lock_surface) {
            wlr_scene_subsurface_tree_create(m_scene_lock_surface, m_lock_surface->surface);
        }
        m_lock_surface_listeners.listen(&m_lock_surface->surface->events.map, this, &Output::on_lock_surface_map);
        m_lock_surface_listeners.listen(&m_lock_surface->events.destroy, this, &Output::on_lock_surface_destroy);
        configure_lock_surface();
    }

    void Output::configure_lock_surface() {
        if (!m_lock_surface) {
            return;
        }
        const wlr_box box = layout_box();
        if (m_scene_lock_surface) {
            wlr_scene_node_set_enabled(&m_scene_lock_surface->node, is_enabled());
            wlr_scene_node_set_position(&m_scene_lock_surface->node, box.x, box.y);
        }
        int eff_width = 0;
        int eff_height = 0;
        if ((box.width <= 0 || box.height <= 0) && m_wlr_output) {
            wlr_output_effective_resolution(m_wlr_output, &eff_width, &eff_height);
        }
        const uint32_t width =
            box.width > 0 ? static_cast<uint32_t>(box.width) : (eff_width > 0 ? static_cast<uint32_t>(eff_width) : 1);
        const uint32_t height = box.height > 0 ? static_cast<uint32_t>(box.height)
                                               : (eff_height > 0 ? static_cast<uint32_t>(eff_height) : 1);
        if (m_lock_configured_width == width && m_lock_configured_height == height) {
            return;
        }
        m_lock_configured_width = width;
        m_lock_configured_height = height;
        wlr_session_lock_surface_v1_configure(m_lock_surface, width, height);
    }

    void Output::destroy_lock_surface() {
        m_lock_surface_listeners.clear();
        m_lock_surface = nullptr;
        m_lock_configured_width = 0;
        m_lock_configured_height = 0;
        if (m_scene_lock_surface) {
            wlr_scene_node_destroy(&m_scene_lock_surface->node);
            m_scene_lock_surface = nullptr;
        }
    }

    void Output::on_lock_surface_map() {
        m_compositor.refocus();
        m_compositor.check_session_locked_done();
    }

    void Output::on_lock_surface_destroy() {
        destroy_lock_surface();
        m_compositor.refocus();
    }

}
