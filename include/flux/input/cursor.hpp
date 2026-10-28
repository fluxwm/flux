#pragma once

#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <variant>

namespace flux {
    class Window;
    class Input;
}

namespace flux::input {

    class Cursor {
    public:
        explicit Cursor(Input& input);
        ~Cursor();

        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;

        [[nodiscard]] bool is_passthrough() const noexcept;
        [[nodiscard]] Window* grabbed_window() const noexcept;
        [[nodiscard]] double x() const noexcept { return m_wlr_cursor ? m_wlr_cursor->x : 0.0; }
        [[nodiscard]] double y() const noexcept { return m_wlr_cursor ? m_wlr_cursor->y : 0.0; }

        void attach_device(wlr_input_device* device) noexcept { wlr_cursor_attach_input_device(m_wlr_cursor, device); }
        void set_xcursor(const char* name) noexcept { wlr_cursor_set_xcursor(m_wlr_cursor, m_cursor_mgr, name); }
        void set_surface(wlr_surface* surface, int32_t hotspot_x, int32_t hotspot_y) noexcept {
            wlr_cursor_set_surface(m_wlr_cursor, surface, hotspot_x, hotspot_y);
        }

        void begin_move(Window* window);
        void begin_resize(Window* window, uint32_t edges);
        void reset_mode();
        void update_focus();

    private:
        struct Passthrough {};

        struct Move {
            Window* window = nullptr;
            double grab_x = 0.0;
            double grab_y = 0.0;
        };

        struct Resize {
            Window* window = nullptr;
            double grab_x = 0.0;
            double grab_y = 0.0;
            wlr_box initial_geometry{};
            uint32_t resize_edges = 0;
        };

        using State = std::variant<Passthrough, Move, Resize>;

        void on_motion(wlr_pointer_motion_event* event);
        void on_motion_absolute(wlr_pointer_motion_absolute_event* event);
        void on_button(wlr_pointer_button_event* event);
        void on_axis(wlr_pointer_axis_event* event);
        void on_frame();

        void process_motion(uint32_t time_msec);
        void process_move(const Move& state);
        void process_resize(const Resize& state);

        Input& m_input;
        wlr_cursor* m_wlr_cursor = nullptr;
        wlr_xcursor_manager* m_cursor_mgr = nullptr;
        State m_state{};
        ListenerGroup m_listeners;
    };

}
