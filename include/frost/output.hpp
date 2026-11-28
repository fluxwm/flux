#pragma once

#include "frost/animation.hpp"
#include "frost/config.hpp"
#include "frost/listener.hpp"
#include "frost/wlroots.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace frost {

    class Compositor;
    class Workspace;
    class Layer;
    enum class Direction : uint8_t;

    class Output {
    public:
        Output(Compositor& compositor, wlr_output* wlr_output);
        ~Output();

        Output(const Output&) = delete;
        Output& operator=(const Output&) = delete;

        [[nodiscard]] Compositor& compositor() const noexcept { return m_compositor; }
        [[nodiscard]] wlr_output* raw() const noexcept { return m_wlr_output; }
        [[nodiscard]] const wlr_box& usable_area() const noexcept { return m_usable_area; }
        void set_usable_area(const wlr_box& area) noexcept { m_usable_area = area; }

        [[nodiscard]] bool matches(const wlr_output* wlr_out) const noexcept { return m_wlr_output == wlr_out; }
        [[nodiscard]] bool is_enabled() const noexcept { return m_wlr_output->enabled; }
        [[nodiscard]] bool is_config_disabled() const noexcept { return m_config_disabled; }
        [[nodiscard]] bool is_dpms_off() const noexcept { return m_dpms_off; }
        void set_dpms_off(bool off) noexcept { m_dpms_off = off; }
        [[nodiscard]] const OutputConfig* runtime_override() const noexcept {
            return m_runtime_override ? &*m_runtime_override : nullptr;
        }
        void apply_runtime_override(OutputConfig cfg) noexcept;
        void sync_file_config(const OutputConfig* cfg);
        [[nodiscard]] wlr_scene_output* scene_output() const noexcept { return m_scene_output; }
        bool apply_config(const OutputConfig* cfg, bool initial = false);
        [[nodiscard]] bool is_nested() const noexcept;
        [[nodiscard]] wlr_box layout_box() const noexcept;
        [[nodiscard]] Output* adjacent_output(Direction direction) const noexcept;

        [[nodiscard]] Workspace& active_workspace() const noexcept { return *m_active_workspace; }
        void set_active_workspace(Workspace* ws) noexcept { m_active_workspace = ws; }
        [[nodiscard]] Workspace& workspace(uint32_t id);
        [[nodiscard]] std::span<const std::unique_ptr<Workspace>> workspaces() const noexcept { return m_workspaces; }
        [[nodiscard]] WorkspaceAnimation& animation() noexcept { return m_animation; }
        [[nodiscard]] const WorkspaceAnimation& animation() const noexcept { return m_animation; }

        void add_layer(wlr_layer_surface_v1* layer_surface);
        void remove_layer(Layer* layer);
        void destroy_layers();
        void arrange_layers();
        [[nodiscard]] std::span<const std::unique_ptr<Layer>> layers() const noexcept { return m_layers; }

        void set_lock_surface(wlr_session_lock_surface_v1* lock_surface);
        void configure_lock_surface();
        void destroy_lock_surface();
        [[nodiscard]] wlr_session_lock_surface_v1* lock_surface() const noexcept { return m_lock_surface; }
        [[nodiscard]] bool is_lock_surface_mapped() const noexcept {
            return m_lock_surface && m_lock_surface->surface && m_lock_surface->surface->mapped;
        }

    private:
        void on_frame();
        void on_request_state(const wlr_output_event_request_state* event);
        void on_destroy();
        void on_lock_surface_map();
        void on_lock_surface_destroy();

        Compositor& m_compositor;
        wlr_output* m_wlr_output = nullptr;
        wlr_scene_output* m_scene_output = nullptr;
        wlr_box m_layout_box{};
        wlr_box m_usable_area{};
        wlr_ext_workspace_group_handle_v1* m_workspace_group = nullptr;
        std::vector<std::unique_ptr<Workspace>> m_workspaces;
        std::vector<std::unique_ptr<Layer>> m_layers;
        Workspace* m_active_workspace = nullptr;
        WorkspaceAnimation m_animation;
        wlr_session_lock_surface_v1* m_lock_surface = nullptr;
        wlr_scene_tree* m_scene_lock_surface = nullptr;
        uint32_t m_lock_configured_width = 0;
        uint32_t m_lock_configured_height = 0;
        bool m_config_disabled = false;
        bool m_dpms_off = false;
        std::optional<OutputConfig> m_file_config;
        std::optional<OutputConfig> m_runtime_override;

        ListenerGroup m_listeners;
        ListenerGroup m_lock_surface_listeners;
    };

}
