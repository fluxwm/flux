#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {

    class Compositor;

    class Output {
    public:
        Output(Compositor& compositor, wlr_output* wlr_output);
        ~Output() = default;

        Output(const Output&) = delete;
        Output& operator=(const Output&) = delete;

        [[nodiscard]] wlr_output* raw() const noexcept { return m_wlr_output; }
        [[nodiscard]] const wlr_box& usable_area() const noexcept { return m_usable_area; }
        void set_usable_area(const wlr_box& area) noexcept { m_usable_area = area; }

        [[nodiscard]] bool matches(const wlr_output* wlr_out) const noexcept { return m_wlr_output == wlr_out; }
        [[nodiscard]] bool is_enabled() const noexcept { return m_wlr_output->enabled; }
        [[nodiscard]] bool is_nested() const noexcept;
        [[nodiscard]] wlr_box layout_box() const noexcept;

    private:
        void on_frame();
        void on_request_state(const wlr_output_event_request_state* event);
        void on_destroy();

        Compositor& m_compositor;
        wlr_output* m_wlr_output = nullptr;
        wlr_scene_output* m_scene_output = nullptr;
        wlr_box m_usable_area{};

        ListenerGroup m_listeners;
    };

}
