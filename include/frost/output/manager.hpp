#pragma once

#include "frost/listener.hpp"
#include "frost/wlroots.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace frost {

    class Compositor;
    class Output;
    class Window;
    struct OutputConfig;

    namespace output {

        class Manager {
        public:
            explicit Manager(Compositor& compositor);
            ~Manager();

            Manager(const Manager&) = delete;
            Manager& operator=(const Manager&) = delete;

            [[nodiscard]] wlr_output_layout* layout() const noexcept { return m_layout; }
            [[nodiscard]] std::span<const std::unique_ptr<Output>> outputs() const noexcept { return m_outputs; }
            [[nodiscard]] Output* from_wlr(wlr_output* wlr_out) const noexcept;
            [[nodiscard]] Output* at(double lx, double ly) const noexcept;
            [[nodiscard]] Output* focused() const noexcept;
            [[nodiscard]] Output* focused_hint() const noexcept { return m_focused_output; }
            void set_focused(Output* output) noexcept { m_focused_output = output; }

            void apply_configs();
            void remove(Output* output);
            void on_enabled_changed(Output* output);
            void update_protocol_config();
            void forget_window(Window* window) noexcept;
            void clear_listeners() noexcept { m_listeners.clear(); }
            void clear_outputs();

        private:
            [[nodiscard]] const OutputConfig* effective_config(const Output* output) const noexcept;
            void arrange_layout();
            void stop_interactions(Output* output);
            void evacuate(Output* output);
            void attach_orphaned_windows(Output* output, bool restore_workspace = false);
            void arrange_and_sync(bool arrange_disabled_workspaces = false);
            void finish_changes(bool had_enabled, std::span<Output* const> newly_disabled,
                                std::span<Output* const> newly_enabled);

            void on_new_output(wlr_output* output);
            void on_power_set_mode(wlr_output_power_v1_set_mode_event* event);
            void on_manager_apply(wlr_output_configuration_v1* config, bool test_only);
            void on_drm_lease_request(wlr_drm_lease_request_v1* request);

            Compositor& m_compositor;
            wlr_output_layout* m_layout = nullptr;
            wlr_output_manager_v1* m_output_mgr = nullptr;
            wlr_drm_lease_v1_manager* m_drm_lease_mgr = nullptr;
            std::vector<std::unique_ptr<Output>> m_outputs;
            Output* m_focused_output = nullptr;
            std::vector<Window*> m_orphaned_focus_order;
            uint32_t m_saved_workspace_id = 1;

            ListenerGroup m_listeners;
        };

    }

}
