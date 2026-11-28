#include "frost/output/manager.hpp"
#include "frost/compositor.hpp"
#include "frost/config.hpp"
#include "frost/input.hpp"
#include "frost/log.hpp"
#include "frost/output.hpp"
#include "frost/scene.hpp"
#include "frost/util.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <utility>

namespace frost::output {

    Manager::Manager(Compositor& compositor)
        : m_compositor(compositor), m_layout(wlr_output_layout_create(compositor.display())) {
        wl_display* display = m_compositor.display();
        wlr_backend* backend = m_compositor.backend();

        wlr_xdg_output_manager_v1_create(display, m_layout);

        auto* output_power_mgr = wlr_output_power_manager_v1_create(display);
        m_listeners.listen(&output_power_mgr->events.set_mode, this, &Manager::on_power_set_mode);

        m_output_mgr = wlr_output_manager_v1_create(display);
        m_listeners.listen(&m_output_mgr->events.apply,
                           [this](void* d) { on_manager_apply(static_cast<wlr_output_configuration_v1*>(d), false); });
        m_listeners.listen(&m_output_mgr->events.test,
                           [this](void* d) { on_manager_apply(static_cast<wlr_output_configuration_v1*>(d), true); });

        m_drm_lease_mgr = wlr_drm_lease_v1_manager_create(display, backend);
        if (m_drm_lease_mgr) {
            m_listeners.listen(&m_drm_lease_mgr->events.request, this, &Manager::on_drm_lease_request);
        }

        m_listeners.listen(&backend->events.new_output, this, &Manager::on_new_output);
    }

    Manager::~Manager() {
        clear_outputs();
        if (m_layout) {
            wlr_output_layout_destroy(m_layout);
        }
    }

    void Manager::clear_outputs() {
        m_listeners.clear();
        m_orphaned_focus_order.clear();
        m_focused_output = nullptr;
        m_outputs.clear();
    }

    void Manager::forget_window(Window* window) noexcept {
        std::erase(m_orphaned_focus_order, window);
    }

    Output* Manager::from_wlr(wlr_output* wlr_out) const noexcept {
        if (!wlr_out) {
            return nullptr;
        }
        auto it = std::ranges::find_if(m_outputs, [wlr_out](const auto& o) { return o->matches(wlr_out); });
        return it != m_outputs.end() ? it->get() : nullptr;
    }

    Output* Manager::at(double lx, double ly) const noexcept {
        Output* out = from_wlr(wlr_output_layout_output_at(m_layout, lx, ly));
        return (out && out->is_enabled()) ? out : nullptr;
    }

    Output* Manager::focused() const noexcept {
        if (m_focused_output && m_focused_output->is_enabled()) {
            return m_focused_output;
        }
        wlr_output* wlr_out = m_compositor.has_input()
                                  ? wlr_output_layout_output_at(m_layout, m_compositor.input().cursor().x(),
                                                                m_compositor.input().cursor().y())
                                  : nullptr;
        if (!wlr_out) {
            wlr_out = wlr_output_layout_get_center_output(m_layout);
        }
        Output* out = from_wlr(wlr_out);
        if (out && out->is_enabled()) {
            return out;
        }
        auto it = std::ranges::find_if(m_outputs, [](const auto& o) { return o->is_enabled(); });
        return it != m_outputs.end() ? it->get() : nullptr;
    }

    const OutputConfig* Manager::effective_config(const Output* output) const noexcept {
        if (!output) {
            return nullptr;
        }
        if (const OutputConfig* ov = output->runtime_override()) {
            return ov;
        }
        return m_compositor.config().output_config(output->raw());
    }

    void Manager::arrange_layout() {
        for (const auto& out : m_outputs) {
            if (out->is_config_disabled()) {
                wlr_output_layout_remove(m_layout, out->raw());
            }
        }

        auto add_to_layout = [&](Output* out, const std::optional<OutputPosition>& pos) {
            const bool was_in_layout = wlr_output_layout_get(m_layout, out->raw()) != nullptr;
            wlr_output_layout_output* l_out = pos ? wlr_output_layout_add(m_layout, out->raw(), pos->x, pos->y)
                                                  : wlr_output_layout_add_auto(m_layout, out->raw());
            if (!was_in_layout && l_out) {
                m_compositor.scene().add_to_output_layout(l_out, out->scene_output());
            }
        };

        for (const auto& out : m_outputs) {
            if (out->is_config_disabled()) {
                continue;
            }
            if (const auto* cfg = effective_config(out.get()); cfg && cfg->position) {
                add_to_layout(out.get(), cfg->position);
            }
        }

        for (const auto& out : m_outputs) {
            if (out->is_config_disabled()) {
                continue;
            }
            const auto* cfg = effective_config(out.get());
            if (!cfg || !cfg->position) {
                add_to_layout(out.get(), std::nullopt);
            }
        }
    }

    void Manager::stop_interactions(Output* output) {
        output->active_workspace().snap_animations();
        if (!m_compositor.cancel_swipe_for(output)) {
            output->animation().cancel();
        }
        if (m_focused_output == output) {
            m_focused_output = nullptr;
        }
    }

    void Manager::evacuate(Output* output) {
        if (!output) {
            return;
        }

        if (m_compositor.has_input()) {
            if (Window* grabbed = m_compositor.input().cursor().grabbed_window();
                grabbed && grabbed->output() == output) {
                m_compositor.input().cursor().cancel_grab();
            }
        }
        output->destroy_layers();
        stop_interactions(output);

        auto it =
            std::ranges::find_if(m_outputs, [output](const auto& o) { return o.get() != output && o->is_enabled(); });
        if (it == m_outputs.end()) {
            it = std::ranges::find_if(
                m_outputs, [output](const auto& o) { return o.get() != output && !o->is_config_disabled(); });
        }
        Output* target = (it != m_outputs.end()) ? it->get() : nullptr;
        if (!target) {
            m_saved_workspace_id = output->active_workspace().id();
        }

        std::vector<Window*> focus_order;
        if (target) {
            for (const auto& ws : std::views::concat(target->workspaces(), output->workspaces())) {
                focus_order.insert(focus_order.end(), ws->focus_stack().begin(), ws->focus_stack().end());
            }
        } else {
            for (const auto& ws : output->workspaces()) {
                for (Window* w : ws->focus_stack()) {
                    if (!std::ranges::contains(m_orphaned_focus_order, w)) {
                        m_orphaned_focus_order.push_back(w);
                    }
                }
            }
        }

        for (const auto& win : m_compositor.windows()) {
            if (win->output() != output) {
                continue;
            }
            if (win->is_mapped()) {
                win->set_activated(false);
            }
            win->set_workspace(target ? &target->workspace(win->workspace_id()) : nullptr);
            win->set_placed(false);
        }

        for (Window* win : std::views::reverse(focus_order)) {
            if (win->workspace()) {
                win->workspace()->promote(win);
            }
        }
    }

    void Manager::attach_orphaned_windows(Output* output, bool restore_workspace) {
        if (!output) {
            return;
        }
        if (restore_workspace && m_saved_workspace_id != 1) {
            output->active_workspace().deactivate();
            output->set_active_workspace(&output->workspace(m_saved_workspace_id));
            output->active_workspace().activate();
            m_saved_workspace_id = 1;
        }
        for (const auto& win : m_compositor.windows()) {
            if (!win->workspace()) {
                win->set_workspace(&output->workspace(win->workspace_id()));
            }
        }
        for (Window* win : std::views::reverse(m_orphaned_focus_order)) {
            if (win->is_mapped() && win->workspace()) {
                win->workspace()->promote(win);
            }
        }
        m_orphaned_focus_order.clear();
    }

    void Manager::arrange_and_sync(bool arrange_disabled_workspaces) {
        for (const auto& out : m_outputs) {
            if (out->is_enabled()) {
                out->arrange_layers();
            } else if (arrange_disabled_workspaces) {
                for (const auto& ws : out->workspaces()) {
                    ws->arrange();
                }
            }
        }

        m_compositor.sync_session_lock();
        m_compositor.refocus();
    }

    void Manager::finish_changes(bool had_enabled, std::span<Output* const> newly_disabled,
                                 std::span<Output* const> newly_enabled) {
        arrange_layout();

        for (Output* out : newly_disabled) {
            evacuate(out);
        }

        bool restore_ws = !had_enabled;
        for (Output* out : newly_enabled) {
            attach_orphaned_windows(out, restore_ws);
            restore_ws = false;
        }

        arrange_and_sync(true);
    }

    void Manager::apply_configs() {
        const bool had_enabled = std::ranges::any_of(m_outputs, &Output::is_enabled);
        std::vector<Output*> newly_disabled;
        std::vector<Output*> newly_enabled;
        for (const auto& out : m_outputs) {
            const bool was_enabled = out->is_enabled();
            const bool was_config_disabled = out->is_config_disabled();
            out->sync_file_config(m_compositor.config().output_config(out->raw()));
            out->apply_config(effective_config(out.get()), false);
            if (!was_config_disabled && out->is_config_disabled()) {
                newly_disabled.push_back(out.get());
            } else if (!was_enabled && out->is_enabled()) {
                newly_enabled.push_back(out.get());
            }
        }

        finish_changes(had_enabled, newly_disabled, newly_enabled);
        update_protocol_config();
    }

    void Manager::remove(Output* output) {
        if (!output) {
            return;
        }

        const bool is_nested = output->is_nested();
        evacuate(output);
        erase_unique_ptr(m_outputs, output);

        if (is_nested && m_outputs.empty()) {
            m_compositor.quit();
            return;
        }

        arrange_layout();
        arrange_and_sync(false);
        update_protocol_config();
    }

    void Manager::on_new_output(wlr_output* output) {
        if (output->non_desktop) {
            if (m_drm_lease_mgr) {
                log_debug("offering non-desktop output '{}' for DRM lease", output->name);
                if (!wlr_drm_lease_v1_manager_offer_output(m_drm_lease_mgr, output)) {
                    log_error("failed to offer non-desktop output '{}' for DRM lease", output->name);
                }
            } else {
                log_debug("ignoring non-desktop output '{}'", output->name);
            }
            return;
        }

        m_outputs.push_back(std::make_unique<Output>(m_compositor, output));
        Output* new_out = m_outputs.back().get();
        arrange_layout();

        if (new_out->is_enabled()) {
            const auto enabled_count =
                static_cast<size_t>(std::ranges::count_if(m_outputs, [](const auto& o) { return o->is_enabled(); }));
            attach_orphaned_windows(new_out, enabled_count == 1);
        }

        arrange_and_sync(false);
        update_protocol_config();
    }

    void Manager::on_enabled_changed(Output* output) {
        if (!output) {
            return;
        }

        if (!output->is_enabled()) {
            stop_interactions(output);
        } else {
            attach_orphaned_windows(output);
        }

        arrange_and_sync(false);
        update_protocol_config();
    }

    void Manager::on_power_set_mode(wlr_output_power_v1_set_mode_event* event) {
        Output* output = from_wlr(event->output);
        if (!output || output->is_config_disabled()) {
            return;
        }

        const bool enable = (event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
        const bool was_enabled = output->is_enabled();
        const bool prev_dpms_off = output->is_dpms_off();
        if (prev_dpms_off == !enable && was_enabled == enable) {
            return;
        }

        output->set_dpms_off(!enable);
        if (!output->apply_config(effective_config(output), false)) {
            output->set_dpms_off(prev_dpms_off);
            return;
        }
        if (output->is_enabled() != was_enabled) {
            on_enabled_changed(output);
        }
    }

    void Manager::update_protocol_config() {
        if (!m_output_mgr) {
            return;
        }
        wlr_output_configuration_v1* config = wlr_output_configuration_v1_create();
        if (!config) {
            return;
        }
        for (const auto& out : m_outputs) {
            wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(config, out->raw());
            if (!head) {
                continue;
            }
            head->state.enabled = !out->is_config_disabled();
            if (head->state.enabled) {
                const wlr_box box = out->layout_box();
                head->state.x = box.x;
                head->state.y = box.y;
            }
        }
        wlr_output_manager_v1_set_configuration(m_output_mgr, config);
    }

    void Manager::on_manager_apply(wlr_output_configuration_v1* config, bool test_only) {
        if (!config) {
            return;
        }

        auto finish_config = [config](bool succeeded) {
            if (succeeded) {
                wlr_output_configuration_v1_send_succeeded(config);
            } else {
                wlr_output_configuration_v1_send_failed(config);
            }
            wlr_output_configuration_v1_destroy(config);
        };

        struct HeadState {
            Output* output = nullptr;
            bool was_enabled = false;
            bool was_config_disabled = false;
        };
        std::vector<HeadState> head_states;
        wlr_output_configuration_head_v1* head = nullptr;
        wl_list_for_each(head, &config->heads, link) {
            Output* out = from_wlr(head->state.output);
            if (!out) {
                finish_config(false);
                return;
            }
            if (head->state.enabled) {
                if (!std::isfinite(head->state.scale) || head->state.scale < 0.1f || head->state.scale > 10.0f) {
                    finish_config(false);
                    return;
                }
                if (!head->state.mode && head->state.custom_mode.width == 0 &&
                    !wl_list_empty(&head->state.output->modes)) {
                    head->state.mode = head->state.output->current_mode ? head->state.output->current_mode
                                                                        : wlr_output_preferred_mode(head->state.output);
                }
            }
            head_states.push_back({
                .output = out,
                .was_enabled = out->is_enabled(),
                .was_config_disabled = out->is_config_disabled(),
            });
        }

        std::vector<wlr_backend_output_state> states(head_states.size());
        size_t idx = 0;
        wl_list_for_each(head, &config->heads, link) {
            states[idx].output = head->state.output;
            wlr_output_state_init(&states[idx].base);
            wlr_output_head_v1_state_apply(&head->state, &states[idx].base);
            ++idx;
        }
        auto finish_states = [&] {
            for (auto& s : states) {
                wlr_output_state_finish(&s.base);
            }
        };

        wlr_backend* backend = m_compositor.backend();
        const bool test_ok = states.empty() || wlr_backend_test(backend, states.data(), states.size());
        if (!test_ok) {
            finish_states();
            finish_config(false);
            return;
        }

        if (test_only) {
            finish_states();
            finish_config(true);
            return;
        }

        const bool had_enabled = std::ranges::any_of(m_outputs, &Output::is_enabled);
        const bool commit_ok = states.empty() || wlr_backend_commit(backend, states.data(), states.size());
        finish_states();
        if (!commit_ok) {
            finish_config(false);
            update_protocol_config();
            return;
        }

        wl_list_for_each(head, &config->heads, link) {
            Output* out = from_wlr(head->state.output);
            OutputConfig override_cfg;
            override_cfg.name = out->raw()->name ? out->raw()->name : "";
            override_cfg.enabled = head->state.enabled;
            override_cfg.scale = head->state.scale;
            override_cfg.transform = head->state.transform;
            if (head->state.enabled) {
                override_cfg.position = OutputPosition{.x = head->state.x, .y = head->state.y};
                if (head->state.mode) {
                    override_cfg.mode = OutputMode{
                        .width = head->state.mode->width,
                        .height = head->state.mode->height,
                        .refresh_mhz =
                            head->state.mode->refresh > 0 ? std::optional{head->state.mode->refresh} : std::nullopt,
                        .custom = false,
                    };
                } else if (head->state.custom_mode.width > 0 && head->state.custom_mode.height > 0) {
                    override_cfg.mode = OutputMode{
                        .width = head->state.custom_mode.width,
                        .height = head->state.custom_mode.height,
                        .refresh_mhz = head->state.custom_mode.refresh > 0
                                           ? std::optional{head->state.custom_mode.refresh}
                                           : std::nullopt,
                        .custom = true,
                    };
                } else if (const auto* prev = effective_config(out); prev && prev->mode) {
                    override_cfg.mode = prev->mode;
                }
            }
            out->apply_runtime_override(std::move(override_cfg));
        }

        std::vector<Output*> newly_disabled;
        std::vector<Output*> newly_enabled;
        for (const auto& entry : head_states) {
            if (!entry.was_config_disabled && entry.output->is_config_disabled()) {
                newly_disabled.push_back(entry.output);
            } else if (!entry.was_enabled && entry.output->is_enabled()) {
                newly_enabled.push_back(entry.output);
            }
        }

        finish_changes(had_enabled, newly_disabled, newly_enabled);
        finish_config(true);
        update_protocol_config();
    }

    void Manager::on_drm_lease_request(wlr_drm_lease_request_v1* request) {
        if (m_compositor.is_session_locked()) {
            log_error("session lock active, rejecting DRM lease request");
            wlr_drm_lease_request_v1_reject(request);
            return;
        }

        if (wlr_drm_lease_request_v1_grant(request)) {
            log_debug("granted DRM lease request");
        } else {
            log_error("failed to grant DRM lease request, rejecting");
            wlr_drm_lease_request_v1_reject(request);
        }
    }

}
