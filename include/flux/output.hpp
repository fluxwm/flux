#pragma once

#include "flux/animation.hpp"
#include "flux/listener.hpp"
#include "flux/wlroots.hpp"

#include <memory>
#include <span>
#include <vector>

namespace flux {

    class Compositor;
    class Workspace;
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
        [[nodiscard]] bool is_nested() const noexcept;
        [[nodiscard]] wlr_box layout_box() const noexcept;
        [[nodiscard]] Output* adjacent_output(Direction direction) const noexcept;

        [[nodiscard]] Workspace& active_workspace() const noexcept { return *m_active_workspace; }
        void set_active_workspace(Workspace* ws) noexcept { m_active_workspace = ws; }
        [[nodiscard]] Workspace& workspace(uint32_t id);
        [[nodiscard]] std::span<const std::unique_ptr<Workspace>> workspaces() const noexcept { return m_workspaces; }
        [[nodiscard]] WorkspaceAnimation& animation() noexcept { return m_animation; }
        [[nodiscard]] const WorkspaceAnimation& animation() const noexcept { return m_animation; }

    private:
        void on_frame();
        void on_request_state(const wlr_output_event_request_state* event);
        void on_destroy();

        Compositor& m_compositor;
        wlr_output* m_wlr_output = nullptr;
        wlr_scene_output* m_scene_output = nullptr;
        wlr_box m_usable_area{};
        wlr_ext_workspace_group_handle_v1* m_workspace_group = nullptr;
        std::vector<std::unique_ptr<Workspace>> m_workspaces;
        Workspace* m_active_workspace = nullptr;
        WorkspaceAnimation m_animation;

        ListenerGroup m_listeners;
    };

}
