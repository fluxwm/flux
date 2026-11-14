#pragma once

#include "flux/config.hpp"
#include "flux/listener.hpp"
#include "flux/scene.hpp"
#include "flux/wlroots.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sys/types.h>
#include <vector>

namespace flux {

    class Output;
    class Window;
    class Popup;
    class Layer;
    class Input;

    struct KeybindResult {
        bool handled = false;
        bool repeat = false;
    };

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
        [[nodiscard]] const Config& config() const noexcept { return *m_config; }
        [[nodiscard]] Input& input() noexcept { return *m_input; }
        [[nodiscard]] const Input& input() const noexcept { return *m_input; }
        [[nodiscard]] wlr_xwayland* xwayland() const noexcept { return m_xwayland; }
        [[nodiscard]] Scene& scene() noexcept { return *m_scene; }
        [[nodiscard]] const Scene& scene() const noexcept { return *m_scene; }
        [[nodiscard]] wlr_output_layout* output_layout() const noexcept { return m_output_layout; }
        [[nodiscard]] Output* output_from_wlr(wlr_output* wlr_out) const noexcept;
        [[nodiscard]] Output* focused_output() const noexcept;
        void set_focused_output(Output* output) noexcept { m_focused_output = output; }
        [[nodiscard]] Layer* exclusive_layer() const noexcept;
        [[nodiscard]] bool allows_tearing(const Output* output) const noexcept;
        [[nodiscard]] wlr_ext_foreign_toplevel_list_v1* foreign_toplevel_list() const noexcept {
            return m_foreign_toplevel_list;
        }
        [[nodiscard]] wlr_ext_workspace_manager_v1* workspace_manager() const noexcept { return m_workspace_manager; }
        [[nodiscard]] Window* focused_window() const noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<Popup>> popups() const noexcept { return m_popups; }
        [[nodiscard]] bool is_workspace_swiping() const noexcept { return m_swipe_output != nullptr; }

        // Window management & interactions
        KeybindResult handle_keybinding(uint32_t modifiers, xkb_keysym_t sym, bool is_repeat = false);
        void focus_window(Window* window);
        void focus_window_in_direction(Direction direction);
        void focus_workspace(uint32_t id) { focus_workspace(focused_output(), id); }
        void focus_workspace(Output* output, uint32_t id);
        void move_to_workspace(Window* window, uint32_t id);
        bool begin_workspace_swipe();
        void update_workspace_swipe(double dx);
        void end_workspace_swipe();
        void cancel_workspace_swipe();
        void cancel_animations();
        void focus_surface(wlr_surface* surface);
        void refocus();
        void focus_layer(Layer* layer);
        void dismiss_popups(wlr_scene_tree* root = nullptr, wlr_scene_tree* except_root = nullptr);
        void update_cursor_focus() noexcept;

        // Collection management
        void map_window(Window* window);
        void unmap_window(Window* window);
        void create_window(wlr_xwayland_surface* xsurface);
        void remove_window(Window* window);
        void create_popup(wlr_xdg_popup* popup);
        void create_popup(wlr_xwayland_surface* xsurface);
        void remove_popup(Popup* popup);
        void recreate_as_popup(Window* window);
        void recreate_as_window(Popup* popup);
        void remove_output(Output* output);
        void on_animation_finished();

    private:
        void restack_windows();

        void on_new_output(wlr_output* output);
        void on_new_window(wlr_xdg_toplevel* toplevel);
        void on_new_layer(wlr_layer_surface_v1* layer_surface);
        void on_request_activate(wlr_xdg_activation_v1_request_activate_event* event);
        void on_workspace_commit(wlr_ext_workspace_v1_commit_event* event);
        void on_output_power_set_mode(wlr_output_power_v1_set_mode_event* event);
        void execute_action(const Action& action);
        void spawn(const action::Spawn& action);

        wl_display* m_display = nullptr;
        wlr_backend* m_backend = nullptr;
        wlr_session* m_session = nullptr;
        wlr_renderer* m_renderer = nullptr;
        wlr_allocator* m_allocator = nullptr;
        wlr_output_layout* m_output_layout = nullptr;
        wlr_ext_foreign_toplevel_list_v1* m_foreign_toplevel_list = nullptr;
        wlr_ext_workspace_manager_v1* m_workspace_manager = nullptr;
        wlr_tearing_control_manager_v1* m_tearing_control_mgr = nullptr;
        wlr_xwayland* m_xwayland = nullptr;

        std::unique_ptr<Config> m_config;
        std::unique_ptr<Scene> m_scene;
        std::unique_ptr<Input> m_input;

        std::vector<std::unique_ptr<Output>> m_outputs;
        Output* m_focused_output = nullptr;
        std::vector<std::unique_ptr<Window>> m_windows;
        std::vector<std::unique_ptr<Popup>> m_popups;
        std::vector<Window*> m_orphaned_focus_order;
        std::vector<pid_t> m_spawned_pids;
        Output* m_swipe_output = nullptr;
        uint32_t m_saved_workspace_id = 1;

        ListenerGroup m_listeners;
        ListenerGroup m_xwayland_listeners;
    };

}
