#pragma once

#include "frost/listener.hpp"
#include "frost/window.hpp"

namespace frost::window {

    class Xdg final : public Window {
    public:
        Xdg(Compositor& compositor, wlr_xdg_toplevel* toplevel);
        ~Xdg() override;

        [[nodiscard]] wlr_surface* surface() const noexcept override;
        [[nodiscard]] bool is_pending_center() const noexcept override { return m_pending_center_serial != 0; }
        [[nodiscard]] bool is_pending_move() const noexcept override { return m_pending_move_serial != 0; }
        void set_tiled_resizing(bool resizing) noexcept override;
        void set_decoration(wlr_xdg_toplevel_decoration_v1* decoration) override;
        void apply_resize(int w, int h) noexcept override;
        void close() noexcept override;

    protected:
        [[nodiscard]] std::pair<int, int> preferred_size() const noexcept override;
        void apply_activated(bool activated) noexcept override;
        void apply_fullscreen(bool fullscreen) noexcept override;
        void apply_tiled(uint32_t edges) noexcept override;
        void schedule_configure() noexcept override;
        [[nodiscard]] bool configure_geometry(const wlr_box& box) noexcept override;
        void start_resizing() noexcept override;
        void end_resizing() noexcept override;
        void clear_pending_serials() noexcept override {
            m_resize_serial = 0;
            m_pending_center_serial = 0;
            m_pending_move_serial = 0;
        }
        void clear_pending_move() noexcept override { m_pending_move_serial = 0; }
        void clear_pending_center() noexcept override { m_pending_center_serial = 0; }
        void request_default_floating_size() noexcept override;
        [[nodiscard]] Window* query_parent() const noexcept override;
        [[nodiscard]] bool wants_floating() const noexcept override;

    private:
        void on_commit();
        void on_request_maximize();
        void on_request_fullscreen();
        void on_set_title();
        void on_set_app_id();
        void on_request_decoration_mode();
        void on_destroy_decoration();

        wlr_xdg_toplevel* m_toplevel = nullptr;
        wlr_xdg_toplevel_decoration_v1* m_decoration = nullptr;
        uint32_t m_resize_serial = 0;
        uint32_t m_pending_center_serial = 0;
        uint32_t m_pending_move_serial = 0;

        ListenerGroup m_listeners;
        ListenerGroup m_decoration_listeners;
    };

}
