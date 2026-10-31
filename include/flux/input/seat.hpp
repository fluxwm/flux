#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {
    class Input;
}

namespace flux::input {

    class Seat {
    public:
        Seat(Input& input, wl_display* display, const char* name = "seat0");
        ~Seat();

        Seat(const Seat&) = delete;
        Seat& operator=(const Seat&) = delete;

        [[nodiscard]] wlr_surface* focused_surface() const noexcept {
            return m_wlr_seat ? m_wlr_seat->keyboard_state.focused_surface : nullptr;
        }
        [[nodiscard]] wlr_keyboard* keyboard() const noexcept {
            return m_wlr_seat ? wlr_seat_get_keyboard(m_wlr_seat) : nullptr;
        }

        void set_capabilities(uint32_t capabilities) noexcept { wlr_seat_set_capabilities(m_wlr_seat, capabilities); }
        void set_keyboard(wlr_keyboard* keyboard) noexcept { wlr_seat_set_keyboard(m_wlr_seat, keyboard); }

        void notify_key(uint32_t time_msec, uint32_t key, uint32_t state) noexcept {
            wlr_seat_keyboard_notify_key(m_wlr_seat, time_msec, key, state);
        }
        void notify_modifiers(wlr_keyboard_modifiers* modifiers) noexcept {
            wlr_seat_keyboard_notify_modifiers(m_wlr_seat, modifiers);
        }
        void notify_keyboard_enter(wlr_surface* surface) noexcept;
        void notify_motion(uint32_t time_msec, double sx, double sy) noexcept {
            wlr_seat_pointer_notify_motion(m_wlr_seat, time_msec, sx, sy);
        }
        void notify_button(uint32_t time_msec, uint32_t button, wl_pointer_button_state state) noexcept {
            wlr_seat_pointer_notify_button(m_wlr_seat, time_msec, button, state);
        }
        void notify_axis(uint32_t time_msec, wl_pointer_axis orientation, double value, int32_t value_discrete,
                         wl_pointer_axis_source source,
                         wl_pointer_axis_relative_direction relative_direction) noexcept {
            wlr_seat_pointer_notify_axis(m_wlr_seat, time_msec, orientation, value, value_discrete, source,
                                         relative_direction);
        }
        void notify_frame() noexcept { wlr_seat_pointer_notify_frame(m_wlr_seat); }
        void notify_enter(wlr_surface* surface, double sx, double sy) noexcept {
            wlr_seat_pointer_notify_enter(m_wlr_seat, surface, sx, sy);
        }
        void clear_pointer_focus() noexcept { wlr_seat_pointer_clear_focus(m_wlr_seat); }

    private:
        void on_request_set_cursor(wlr_seat_pointer_request_set_cursor_event* event);
        void on_request_set_shape(wlr_cursor_shape_manager_v1_request_set_shape_event* event);
        void on_pointer_focus_change(wlr_seat_pointer_focus_change_event* event);
        void on_request_set_selection(wlr_seat_request_set_selection_event* event);
        void clear_keyboard_focus() noexcept { wlr_seat_keyboard_notify_clear_focus(m_wlr_seat); }

        Input& m_input;
        wlr_seat* m_wlr_seat = nullptr;
        ListenerGroup m_listeners;
    };

}
