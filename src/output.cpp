#include "flux/output.hpp"
#include "flux/compositor.hpp"
#include "flux/config.hpp"
#include "flux/layer.hpp"
#include "flux/popup.hpp"
#include "flux/util.hpp"
#include "flux/workspace.hpp"

#include <algorithm>
#include <array>
#include <ctime>

namespace flux {

    Output::Output(Compositor& compositor, wlr_output* wlr_output)
        : m_compositor(compositor), m_wlr_output(wlr_output) {
        wlr_output_init_render(m_wlr_output, m_compositor.allocator(), m_compositor.renderer());

        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);

        wlr_output_mode* mode = wlr_output_preferred_mode(m_wlr_output);
        if (mode != nullptr) {
            wlr_output_state_set_mode(&state, mode);
        }

        wlr_output_commit_state(m_wlr_output, &state);
        wlr_output_state_finish(&state);

        m_listeners.listen(&m_wlr_output->events.frame, this, &Output::on_frame);
        m_listeners.listen(&m_wlr_output->events.request_state, this, &Output::on_request_state);
        m_listeners.listen(&m_wlr_output->events.destroy, this, &Output::on_destroy);

        wlr_output_layout_output* l_output = wlr_output_layout_add_auto(m_compositor.output_layout(), m_wlr_output);
        m_scene_output = m_compositor.scene().attach_output(m_wlr_output, l_output);

        if (wlr_output_is_wl(m_wlr_output)) {
            wlr_wl_output_set_title(m_wlr_output, "Flux");
            wlr_wl_output_set_app_id(m_wlr_output, "flux");
        } else if (wlr_output_is_x11(m_wlr_output)) {
            wlr_x11_output_set_title(m_wlr_output, "Flux");
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
        m_listeners.clear();
        m_animation.cancel();
        for (auto& layer : m_layers) {
            wlr_layer_surface_v1* raw = layer->raw();
            layer.reset();
            wlr_layer_surface_v1_destroy(raw);
        }
        m_layers.clear();
        m_workspaces.clear();
        if (m_workspace_group) {
            wlr_ext_workspace_group_handle_v1_destroy(m_workspace_group);
            m_workspace_group = nullptr;
        }
        if (m_scene_output) {
            wlr_scene_output_destroy(m_scene_output);
            m_scene_output = nullptr;
        }
        if (m_wlr_output && m_compositor.output_layout()) {
            wlr_output_layout_remove(m_compositor.output_layout(), m_wlr_output);
            m_wlr_output = nullptr;
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

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(m_scene_output, &now);
    }

    void Output::on_request_state(const wlr_output_event_request_state* event) {
        if (wlr_output_commit_state(m_wlr_output, event->state)) {
            arrange_layers();
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

    void Output::arrange_layers() {
        const wlr_box output_box = layout_box();
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

        for (const auto& popup : m_compositor.popups()) {
            popup->update_position();
        }

        for (const auto& ws : m_workspaces) {
            ws->arrange();
        }

        m_compositor.update_cursor_focus();
    }

}
