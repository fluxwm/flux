#pragma once

#include "flux/config.hpp"
#include "flux/cursor.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace flux {

    class Output;
    class Keyboard;
    class Window;
    class Popup;

    class Compositor {
    public:
        Compositor();
        ~Compositor();

        Compositor(const Compositor&) = delete;
        Compositor& operator=(const Compositor&) = delete;

        bool init(std::optional<std::filesystem::path> config_path = std::nullopt);
        void run();
        void quit() noexcept;

        // Core accessors
        [[nodiscard]] wl_display* display() const noexcept { return m_display; }
        [[nodiscard]] wlr_backend* backend() const noexcept { return m_backend; }
        [[nodiscard]] wlr_renderer* renderer() const noexcept { return m_renderer; }
        [[nodiscard]] wlr_allocator* allocator() const noexcept { return m_allocator; }
        [[nodiscard]] wlr_scene* scene() const noexcept { return m_scene; }
        [[nodiscard]] wlr_scene_output_layout* scene_layout() const noexcept { return m_scene_layout; }
        [[nodiscard]] wlr_output_layout* output_layout() const noexcept { return m_output_layout; }
        [[nodiscard]] wlr_seat* seat() const noexcept { return m_seat; }
        [[nodiscard]] wlr_cursor* cursor() const noexcept { return m_cursor; }
        [[nodiscard]] wlr_xcursor_manager* cursor_mgr() const noexcept { return m_cursor_mgr; }

        // Window management & interactions
        bool handle_keybinding(uint32_t modifiers, xkb_keysym_t sym);
        void focus_window(Window* window);
        void center_window(Window* window);
        void begin_move(Window* window);
        void begin_resize(Window* window, uint32_t edges);
        void reset_cursor_mode();
        [[nodiscard]] Window* grabbed_window() const noexcept;

        // Collection management
        void map_window(Window* window);
        void unmap_window(Window* window);
        void remove_window(Window* window);
        void remove_popup(Popup* popup);
        void remove_output(Output* output);
        void remove_keyboard(Keyboard* keyboard);

    private:
        void on_new_output(wlr_output* output);
        void on_new_window(wlr_xdg_toplevel* toplevel);
        void on_new_popup(wlr_xdg_popup* popup);
        void on_cursor_motion(wlr_pointer_motion_event* event);
        void on_cursor_motion_absolute(wlr_pointer_motion_absolute_event* event);
        void on_cursor_button(wlr_pointer_button_event* event);
        void on_cursor_axis(wlr_pointer_axis_event* event);
        void on_cursor_frame();
        void on_new_input(wlr_input_device* device);
        void on_request_cursor(wlr_seat_pointer_request_set_cursor_event* event);
        void on_pointer_focus_change(wlr_seat_pointer_focus_change_event* event);
        void on_request_set_selection(wlr_seat_request_set_selection_event* event);

        void process_cursor_motion(uint32_t time_msec);
        void process_cursor_move(const cursor_state::Move& state);
        void process_cursor_resize(const cursor_state::Resize& state);
        Window* window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy);
        void execute_action(const Action& action);
        void spawn(const action::Spawn& action);

        wl_display* m_display = nullptr;
        wlr_backend* m_backend = nullptr;
        wlr_session* m_session = nullptr;
        wlr_renderer* m_renderer = nullptr;
        wlr_allocator* m_allocator = nullptr;
        wlr_scene* m_scene = nullptr;
        wlr_scene_output_layout* m_scene_layout = nullptr;
        wlr_output_layout* m_output_layout = nullptr;
        wlr_xdg_shell* m_xdg_shell = nullptr;
        wlr_cursor* m_cursor = nullptr;
        wlr_xcursor_manager* m_cursor_mgr = nullptr;
        wlr_seat* m_seat = nullptr;

        Config m_config;
        CursorState m_cursor_state{cursor_state::Passthrough{}};

        std::vector<std::unique_ptr<Output>> m_outputs;
        std::vector<std::unique_ptr<Keyboard>> m_keyboards;
        std::vector<std::unique_ptr<Window>> m_windows;
        std::vector<std::unique_ptr<Popup>> m_popups;
        std::vector<Window*> m_focus_stack;

        ListenerGroup m_listeners;
    };

}
