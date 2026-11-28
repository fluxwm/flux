#pragma once

#include "frost/config.hpp"
#include "frost/listener.hpp"
#include "frost/wlroots.hpp"

#include <memory>
#include <optional>
#include <string>

namespace frost {
    class Input;
    class Window;
    class Workspace;
}

namespace frost::input {

    class Grab;

    class Cursor {
    public:
        explicit Cursor(Input& input);
        ~Cursor();

        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;

        [[nodiscard]] double x() const noexcept { return m_wlr_cursor ? m_wlr_cursor->x : 0.0; }
        [[nodiscard]] double y() const noexcept { return m_wlr_cursor ? m_wlr_cursor->y : 0.0; }
        [[nodiscard]] const char* effective_theme() const noexcept {
            if (!m_config.theme.empty()) {
                return m_config.theme.c_str();
            }
            return m_env_xcursor_theme ? m_env_xcursor_theme->c_str() : nullptr;
        }

        void attach_device(wlr_input_device* device) noexcept { wlr_cursor_attach_input_device(m_wlr_cursor, device); }
        void map_to_output(wlr_input_device* device, wlr_output* output) noexcept {
            wlr_cursor_map_input_to_output(m_wlr_cursor, device, output);
        }
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
        void clear_grab() noexcept;
        void clear_button_grab() noexcept { m_button_grab = false; }
        [[nodiscard]] Window* grabbed_window() const noexcept;
        [[nodiscard]] bool is_tiled_resizing(const Workspace* ws) const noexcept;
        void apply_config(const CursorConfig& config);
        void set_xwayland_cursor(wlr_xwayland* xwayland);

    private:
        void on_motion(wlr_pointer_motion_event* event);
        void on_motion_absolute(wlr_pointer_motion_absolute_event* event);
        void handle_motion(wlr_input_device* device, uint32_t time_msec, double dx, double dy, double unaccel_dx,
                           double unaccel_dy);
        void on_button(wlr_pointer_button_event* event);
        void on_axis(wlr_pointer_axis_event* event);
        void on_frame();
        void on_swipe_begin(wlr_pointer_swipe_begin_event* event);
        void on_swipe_update(wlr_pointer_swipe_update_event* event);
        void on_swipe_end(wlr_pointer_swipe_end_event* event);

        void start_grab(Window* window, uint32_t button);
        void end_grab(bool commit = true);
        void toggle_grab_floating();
        [[nodiscard]] bool resolve_button_grab(int* ox = nullptr, int* oy = nullptr) const noexcept;
        bool apply_constraint(double& dx, double& dy) const;
        void process_motion(uint32_t time_msec);

        Input& m_input;
        wlr_cursor* m_wlr_cursor = nullptr;
        wlr_xcursor_manager* m_cursor_mgr = nullptr;
        CursorConfig m_config{};
        const char* m_current_shape = "default";
        std::optional<std::string> m_env_xcursor_theme;
        std::unique_ptr<Grab> m_grab;
        bool m_button_grab = false;
        wlr_pointer_gestures_v1* m_pointer_gestures = nullptr;
        wlr_relative_pointer_manager_v1* m_relative_pointer_mgr = nullptr;
        bool m_swiping_workspace = false;
        ListenerGroup m_listeners;
    };

}
