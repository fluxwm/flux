#pragma once

#include "flux/config.hpp"
#include "flux/listener.hpp"
#include "flux/scene.hpp"
#include "flux/wlroots.hpp"
#include "flux/workspace.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace flux {

    class Output;
    class Window;
    class Popup;
    class Layer;
    class Input;

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
        [[nodiscard]] Scene& scene() noexcept { return *m_scene; }
        [[nodiscard]] const Scene& scene() const noexcept { return *m_scene; }
        [[nodiscard]] wlr_output_layout* output_layout() const noexcept { return m_output_layout; }
        [[nodiscard]] Output* output_from_wlr(wlr_output* wlr_out) const noexcept;
        [[nodiscard]] Output* focused_output() const noexcept;
        [[nodiscard]] Output* window_output(const Window* window) const noexcept;
        [[nodiscard]] Layer* exclusive_layer() const noexcept;
        [[nodiscard]] wlr_ext_foreign_toplevel_list_v1* foreign_toplevel_list() const noexcept {
            return m_foreign_toplevel_list;
        }
        [[nodiscard]] Window* focused_window() const noexcept {
            return m_current_workspace ? m_current_workspace->focused_window() : nullptr;
        }
        [[nodiscard]] Workspace& current_workspace() const noexcept { return *m_current_workspace; }

        // Window management & interactions
        bool handle_keybinding(uint32_t modifiers, xkb_keysym_t sym);
        void focus_window(Window* window);
        void focus_workspace(uint32_t id);
        void move_to_workspace(Window* window, uint32_t id);
        void focus_surface(wlr_surface* surface);
        void center_window(Window* window);
        void begin_move(Window* window);
        void begin_resize(Window* window, uint32_t edges);
        void refocus();
        void arrange_layers(Output* output);
        void focus_layer(Layer* layer);
        void create_popup(wlr_xdg_popup* popup);

        // Collection management
        void map_window(Window* window);
        void unmap_window(Window* window);
        void remove_window(Window* window);
        void remove_popup(Popup* popup);
        void remove_output(Output* output);
        void remove_layer(Layer* layer);

    private:
        Workspace& workspace(uint32_t id);

        void on_new_output(wlr_output* output);
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
        wlr_output_layout* m_output_layout = nullptr;
        wlr_ext_foreign_toplevel_list_v1* m_foreign_toplevel_list = nullptr;
        wlr_ext_workspace_group_handle_v1* m_workspace_group = nullptr;

        std::unique_ptr<Config> m_config;
        std::unique_ptr<Scene> m_scene;
        std::unique_ptr<Input> m_input;

        std::vector<std::unique_ptr<Output>> m_outputs;
        std::vector<std::unique_ptr<Window>> m_windows;
        std::vector<std::unique_ptr<Popup>> m_popups;
        std::vector<std::unique_ptr<Layer>> m_layers;
        std::vector<std::unique_ptr<Workspace>> m_workspaces;
        Workspace* m_current_workspace = nullptr;

        ListenerGroup m_listeners;
    };

}
