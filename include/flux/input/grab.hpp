#pragma once

#include "flux/wlroots.hpp"

#include <cmath>
#include <cstdint>

namespace flux {
    class Window;
    class Workspace;
}

namespace flux::input {

    class Grab {
    public:
        Grab(Window* window, uint32_t button, double cursor_x, double cursor_y) noexcept
            : m_window(window), m_button(button), m_cursor_start_x(cursor_x), m_cursor_start_y(cursor_y) {}
        virtual ~Grab() = default;

        Grab(const Grab&) = delete;
        Grab& operator=(const Grab&) = delete;
        Grab(Grab&&) = delete;
        Grab& operator=(Grab&&) = delete;

        [[nodiscard]] Window* window() const noexcept { return m_window; }
        [[nodiscard]] uint32_t button() const noexcept { return m_button; }

        void on_motion(double cursor_x, double cursor_y) {
            const int dx = static_cast<int>(std::round(cursor_x - m_cursor_start_x));
            const int dy = static_cast<int>(std::round(cursor_y - m_cursor_start_y));
            update(dx, dy);
        }

        virtual void start() = 0;
        virtual void update(int dx, int dy) = 0;
        virtual void end(bool commit) = 0;
        [[nodiscard]] virtual bool is_valid() const;
        [[nodiscard]] virtual const char* cursor_name() const noexcept = 0;
        [[nodiscard]] virtual const Workspace* resized_workspace() const noexcept { return nullptr; }
        [[nodiscard]] virtual bool involves_window(const Window* window) const noexcept { return m_window == window; }

    protected:
        Window* m_window;
        uint32_t m_button;
        double m_cursor_start_x;
        double m_cursor_start_y;
    };

    class MoveGrab final : public Grab {
    public:
        using Grab::Grab;
        ~MoveGrab() override = default;

        void start() override;
        void update(int dx, int dy) override;
        void end(bool commit) override;
        [[nodiscard]] bool is_valid() const override;
        [[nodiscard]] const char* cursor_name() const noexcept override { return "grabbing"; }

    private:
        wlr_box m_initial_box{};
    };

    class ResizeGrab final : public Grab {
    public:
        ResizeGrab(Window* window, uint32_t button, double cursor_x, double cursor_y, uint32_t edges) noexcept
            : Grab(window, button, cursor_x, cursor_y), m_edges(edges) {}
        ~ResizeGrab() override = default;

        void start() override;
        void update(int dx, int dy) override;
        void end(bool commit) override;
        [[nodiscard]] bool is_valid() const override;
        [[nodiscard]] const char* cursor_name() const noexcept override;

    private:
        uint32_t m_edges = WLR_EDGE_NONE;
        wlr_box m_initial_box{};
    };

    class TiledResizeGrab final : public Grab {
    public:
        TiledResizeGrab(Window* window, uint32_t button, double cursor_x, double cursor_y, uint32_t edges) noexcept;
        ~TiledResizeGrab() override = default;

        void start() override;
        void update(int dx, int dy) override;
        void end(bool commit) override;
        [[nodiscard]] bool is_valid() const override;
        [[nodiscard]] const char* cursor_name() const noexcept override;
        [[nodiscard]] const Workspace* resized_workspace() const noexcept override { return &m_workspace; }
        [[nodiscard]] bool involves_window(const Window* window) const noexcept override;

    private:
        Workspace& m_workspace;
        uint32_t m_edges = WLR_EDGE_NONE;
        float m_initial_master_factor = 0.5f;
        Window* m_top_win = nullptr;
        Window* m_bot_win = nullptr;
        float m_top_weight = 1.0f;
        float m_bot_weight = 1.0f;
    };

}
