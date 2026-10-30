#include "flux/workspace.hpp"
#include "flux/scene.hpp"
#include "flux/window.hpp"

#include <algorithm>
#include <string>

namespace flux {

    Workspace::Workspace(uint32_t id, Scene& scene, wlr_ext_workspace_group_handle_v1* group)
        : m_id(id), m_tree(wlr_scene_tree_create(scene.windows())) {
        wlr_scene_node_set_enabled(&m_tree->node, false);

        const std::string id_str = std::to_string(id);
        constexpr uint32_t caps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
        m_handle = wlr_ext_workspace_handle_v1_create(group->manager, id_str.c_str(), caps);
        m_handle->data = this;
        wlr_ext_workspace_handle_v1_set_group(m_handle, group);
        wlr_ext_workspace_handle_v1_set_name(m_handle, id_str.c_str());
        wlr_ext_workspace_handle_v1_set_coordinates(m_handle, &m_id, 1);
    }

    Workspace::~Workspace() {
        wlr_ext_workspace_handle_v1_destroy(m_handle);
    }

    void Workspace::activate() noexcept {
        wlr_scene_node_set_enabled(&m_tree->node, true);
        wlr_ext_workspace_handle_v1_set_active(m_handle, true);
    }

    void Workspace::deactivate() noexcept {
        for (Window* w : m_focus_stack) {
            if (w->is_fullscreen()) {
                w->reparent_to_workspace();
            }
        }
        wlr_scene_node_set_enabled(&m_tree->node, false);
        wlr_ext_workspace_handle_v1_set_active(m_handle, false);
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
