#pragma once

#include "flux/config.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

namespace flux {
    class Input;
}

namespace flux::input {

    class Cursor {
    public:
        explicit Cursor(Input& input);
        ~Cursor();

        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;

        [[nodiscard]] double x() const noexcept { return m_wlr_cursor ? m_wlr_cursor->x : 0.0; }
        [[nodiscard]] double y() const noexcept { return m_wlr_cursor ? m_wlr_cursor->y : 0.0; }

        void attach_device(wlr_input_device* device) noexcept { wlr_cursor_attach_input_device(m_wlr_cursor, device); }
        void set_xcursor(const char* name) noexcept { wlr_cursor_set_xcursor(m_wlr_cursor, m_cursor_mgr, name); }
        void set_surface(wlr_surface* surface, int32_t hotspot_x, int32_t hotspot_y) noexcept {
            wlr_cursor_set_surface(m_wlr_cursor, surface, hotspot_x, hotspot_y);
        }

        void update_focus();
        void apply_config(const CursorConfig& config);

    private:
        void on_motion(wlr_pointer_motion_event* event);
        void on_motion_absolute(wlr_pointer_motion_absolute_event* event);
        void on_button(wlr_pointer_button_event* event);
        void on_axis(wlr_pointer_axis_event* event);
        void on_frame();

        void process_motion(uint32_t time_msec);

        Input& m_input;
        wlr_cursor* m_wlr_cursor = nullptr;
        wlr_xcursor_manager* m_cursor_mgr = nullptr;
        CursorConfig m_config{};
        ListenerGroup m_listeners;
    };

}
