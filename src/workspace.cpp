#include "flux/workspace.hpp"
#include "flux/scene.hpp"
#include "flux/window.hpp"

#include <algorithm>

namespace flux {

    Workspace::Workspace(uint32_t id, Scene& scene, bool active)
        : m_id(id), m_tree(wlr_scene_tree_create(scene.windows())) {
        wlr_scene_node_set_enabled(&m_tree->node, active);
    }

    void Workspace::activate() noexcept {
        wlr_scene_node_set_enabled(&m_tree->node, true);
    }

    void Workspace::deactivate() noexcept {
        for (Window* w : m_focus_stack) {
            if (w->is_fullscreen()) {
                w->reparent_to_workspace();
            }
        }
        wlr_scene_node_set_enabled(&m_tree->node, false);
    }

    void Workspace::promote(Window* window) {
        if (!window || focused_window() == window) {
            return;
        }
        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);
    }

    void Workspace::remove(Window* window) noexcept {
        std::erase(m_focus_stack, window);
    }

    Window* Workspace::cycle() {
        if (m_focus_stack.size() < 2) {
            return nullptr;
        }
        std::ranges::rotate(m_focus_stack, m_focus_stack.begin() + 1);
        return m_focus_stack.front();
    }

}
