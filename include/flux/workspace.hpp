#pragma once

#include "flux/wlroots.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace flux {

    class Scene;
    class Window;

    class Workspace {
    public:
        Workspace(uint32_t id, Scene& scene, bool active);
        ~Workspace() = default;

        Workspace(const Workspace&) = delete;
        Workspace& operator=(const Workspace&) = delete;

        [[nodiscard]] uint32_t id() const noexcept { return m_id; }
        [[nodiscard]] wlr_scene_tree* tree() const noexcept { return m_tree; }
        [[nodiscard]] std::span<Window* const> focus_stack() const noexcept { return m_focus_stack; }
        [[nodiscard]] Window* focused_window() const noexcept {
            return m_focus_stack.empty() ? nullptr : m_focus_stack.front();
        }

        void activate() noexcept;
        void deactivate() noexcept;

        void promote(Window* window);
        void remove(Window* window) noexcept;
        Window* cycle();

    private:
        uint32_t m_id = 1;
        wlr_scene_tree* m_tree = nullptr;
        std::vector<Window*> m_focus_stack;
    };

}
