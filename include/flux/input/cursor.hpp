#pragma once

#include "flux/config.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <optional>
#include <string>

namespace flux {
    class Input;
    class Window;
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
        void set_xcursor(const char* name) noexcept {
            m_current_shape = (name && *name != '\0') ? name : "default";
            wlr_cursor_set_xcursor(m_wlr_cursor, m_cursor_mgr, m_current_shape);
        }
        void set_surface(wlr_surface* surface, int32_t hotspot_x, int32_t hotspot_y) noexcept {
            m_current_shape = nullptr;
            wlr_cursor_set_surface(m_wlr_cursor, surface, hotspot_x, hotspot_y);
        }
        void warp(double lx, double ly) noexcept { wlr_cursor_warp(m_wlr_cursor, nullptr, lx, ly); }

        void update_focus();
        void cancel_grab(const Window* window = nullptr);
        void apply_config(const CursorConfig& config);

    private:
        void on_motion(wlr_pointer_motion_event* event);
        void on_motion_absolute(wlr_pointer_motion_absolute_event* event);
        void on_button(wlr_pointer_button_event* event);
        void on_axis(wlr_pointer_axis_event* event);
        void on_frame();

        void start_grab(Window* window, uint32_t button);
        void end_grab();
        bool apply_constraint(double& dx, double& dy) const;
        void process_motion(uint32_t time_msec);

        Input& m_input;
        wlr_cursor* m_wlr_cursor = nullptr;
        wlr_xcursor_manager* m_cursor_mgr = nullptr;
        CursorConfig m_config{};
        const char* m_current_shape = "default";
        std::optional<std::string> m_env_xcursor_theme;
        Window* m_grab_window = nullptr;
        uint32_t m_grab_button = 0;
        double m_grab_x = 0.0;
        double m_grab_y = 0.0;
        ListenerGroup m_listeners;
    };

}
