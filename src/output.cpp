#include "flux/output.hpp"
#include "flux/compositor.hpp"

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

        m_compositor.arrange_layers(this);
    }

    void Output::on_frame() {
        if (!m_scene_output || !wlr_scene_output_commit(m_scene_output, nullptr)) {
            return;
        }

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(m_scene_output, &now);
    }

    void Output::on_request_state(const wlr_output_event_request_state* event) {
        if (wlr_output_commit_state(m_wlr_output, event->state)) {
            m_compositor.arrange_layers(this);
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

}
