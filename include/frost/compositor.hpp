#pragma once

#include "frost/config.hpp"
#include "frost/listener.hpp"
#include "frost/lock.hpp"
#include "frost/output/manager.hpp"
#include "frost/scene.hpp"
#include "frost/wlroots.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sys/types.h>
#include <vector>

namespace frost {

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
        [[nodiscard]] bool has_input() const noexcept { return m_input != nullptr; }
        [[nodiscard]] Input& input() noexcept { return *m_input; }
        [[nodiscard]] const Input& input() const noexcept { return *m_input; }
        [[nodiscard]] wlr_xwayland* xwayland() const noexcept { return m_xwayland; }
        [[nodiscard]] Scene& scene() noexcept { return *m_scene; }
        [[nodiscard]] const Scene& scene() const noexcept { return *m_scene; }
        [[nodiscard]] wlr_output_layout* output_layout() const noexcept {
            return m_output_manager ? m_output_manager->layout() : nullptr;
        }
        [[nodiscard]] Output* output_from_wlr(wlr_output* wlr_out) const noexcept {
            return m_output_manager ? m_output_manager->from_wlr(wlr_out) : nullptr;
        }
        [[nodiscard]] Output* output_at(double lx, double ly) const noexcept {
            return m_output_manager ? m_output_manager->at(lx, ly) : nullptr;
        }
        [[nodiscard]] Output* focused_output() const noexcept {
            return m_output_manager ? m_output_manager->focused() : nullptr;
        }
        void set_focused_output(Output* output) noexcept {
            if (m_output_manager) {
                m_output_manager->set_focused(output);
            }
        }
        [[nodiscard]] std::span<const std::unique_ptr<Output>> outputs() const noexcept {
            return m_output_manager ? m_output_manager->outputs() : std::span<const std::unique_ptr<Output>>{};
        }
        [[nodiscard]] Layer* exclusive_layer() const noexcept;
        [[nodiscard]] bool is_session_locked() const noexcept { return m_lock && m_lock->is_locked(); }
        [[nodiscard]] bool is_locked() const noexcept { return is_session_locked() || exclusive_layer() != nullptr; }
        [[nodiscard]] Output* output_from_lock_surface(wlr_surface* surface) const noexcept {
            return m_lock ? m_lock->output_from_surface(surface) : nullptr;
        }
        void sync_session_lock() {
            if (m_lock) {
                m_lock->sync();
            }
        }
        void check_session_locked_done() {
            if (m_lock) {
                m_lock->try_send_locked();
            }
        }
        [[nodiscard]] bool allows_tearing(const Output* output) const noexcept;
        [[nodiscard]] wlr_ext_foreign_toplevel_list_v1* foreign_toplevel_list() const noexcept {
            return m_foreign_toplevel_list;
        }
        [[nodiscard]] wlr_ext_workspace_manager_v1* workspace_manager() const noexcept { return m_workspace_manager; }
        [[nodiscard]] Window* focused_window() const noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<Window>> windows() const noexcept { return m_windows; }
        [[nodiscard]] std::span<const std::unique_ptr<Popup>> popups() const noexcept { return m_popups; }
        [[nodiscard]] bool is_workspace_swiping() const noexcept { return m_swipe_output != nullptr; }

        // Window management & interactions
        KeybindResult handle_keybinding(uint32_t modifiers, xkb_keysym_t sym, bool is_repeat = false);
        void focus_window(Window* window);
        void focus_window_in_direction(Direction direction);
        void swap_window_in_direction(Direction direction);
        void focus_workspace(uint32_t id) { focus_workspace(focused_output(), id); }
        void focus_workspace(Output* output, uint32_t id);
        void move_to_workspace(Window* window, uint32_t id);
        bool begin_workspace_swipe();
        void update_workspace_swipe(double dx);
        void end_workspace_swipe();
        void cancel_workspace_swipe();
        bool cancel_swipe_for(Output* output);
        void cancel_animations();
        void focus_surface(wlr_surface* surface);
        void refocus();
        void focus_layer(Layer* layer);
        void dismiss_popups(wlr_scene_tree* root = nullptr, wlr_scene_tree* except_root = nullptr);
        void update_popup_positions() noexcept;
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
        void remove_output(Output* output) {
            if (m_output_manager) {
                m_output_manager->remove(output);
            }
        }
        void on_output_enabled_changed(Output* output) {
            if (m_output_manager) {
                m_output_manager->on_enabled_changed(output);
            }
        }
        void update_output_manager_config() {
            if (m_output_manager) {
                m_output_manager->update_protocol_config();
            }
        }
        void on_animation_finished();

    private:
        void restack_windows();

        void on_new_window(wlr_xdg_toplevel* toplevel);
        void on_new_layer(wlr_layer_surface_v1* layer_surface);
        void on_request_activate(wlr_xdg_activation_v1_request_activate_event* event);
        void on_workspace_commit(wlr_ext_workspace_v1_commit_event* event);
        void execute_action(const Action& action);
        void spawn(const action::Spawn& action);

        wl_display* m_display = nullptr;
        wlr_backend* m_backend = nullptr;
        wlr_session* m_session = nullptr;
        wlr_renderer* m_renderer = nullptr;
        wlr_allocator* m_allocator = nullptr;
        wlr_ext_foreign_toplevel_list_v1* m_foreign_toplevel_list = nullptr;
        wlr_ext_workspace_manager_v1* m_workspace_manager = nullptr;
        wlr_tearing_control_manager_v1* m_tearing_control_mgr = nullptr;
        wlr_xwayland* m_xwayland = nullptr;

        std::unique_ptr<Config> m_config;
        std::unique_ptr<output::Manager> m_output_manager;
        std::unique_ptr<Scene> m_scene;
        std::unique_ptr<Input> m_input;
        std::unique_ptr<Lock> m_lock;

        std::vector<std::unique_ptr<Window>> m_windows;
        std::vector<std::unique_ptr<Popup>> m_popups;
        std::vector<pid_t> m_spawned_pids;
        std::vector<wl_event_source*> m_signal_sources;
        Output* m_swipe_output = nullptr;

        ListenerGroup m_listeners;
        ListenerGroup m_xwayland_listeners;
    };

}
