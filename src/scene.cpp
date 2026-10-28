#include "flux/scene.hpp"
#include "flux/window.hpp"

namespace flux {

    Scene::Scene(wl_display* display, wlr_output_layout* output_layout) {
        m_scene = wlr_scene_create();
        m_scene_background = wlr_scene_tree_create(&m_scene->tree);
        m_scene_bottom = wlr_scene_tree_create(&m_scene->tree);
        m_scene_windows = wlr_scene_tree_create(&m_scene->tree);
        m_scene_top = wlr_scene_tree_create(&m_scene->tree);
        m_scene_fullscreen = wlr_scene_tree_create(&m_scene->tree);
        m_scene_overlay = wlr_scene_tree_create(&m_scene->tree);
        m_scene_popups = wlr_scene_tree_create(&m_scene->tree);
        m_scene_layout = wlr_scene_attach_output_layout(m_scene, output_layout);

        wlr_scene_set_gamma_control_manager_v1(m_scene, wlr_gamma_control_manager_v1_create(display));
    }

    Scene::~Scene() {
        if (m_scene) {
            wlr_scene_node_destroy(&m_scene->tree.node);
        }
    }

    wlr_scene_tree* Scene::layer_tree(zwlr_layer_shell_v1_layer layer) const noexcept {
        switch (layer) {
            case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
                return m_scene_background;
            case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
                return m_scene_bottom;
            case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
                return m_scene_top;
            case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
                return m_scene_overlay;
        }
        return m_scene_top;
    }

    wlr_scene_output* Scene::attach_output(wlr_output* output, wlr_output_layout_output* layout_output) {
        if (!m_scene || !output) {
            return nullptr;
        }
        wlr_scene_output* scene_output = wlr_scene_output_create(m_scene, output);
        if (scene_output && layout_output && m_scene_layout) {
            wlr_scene_output_layout_add_output(m_scene_layout, layout_output, scene_output);
        }
        return scene_output;
    }

    wlr_scene_node* Scene::surface_node_at(double lx, double ly, double* sx, double* sy, wlr_surface** surface) const {
        wlr_scene_node* node = wlr_scene_node_at(&m_scene->tree.node, lx, ly, sx, sy);
        if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
            return nullptr;
        }

        wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
        if (!scene_surface) {
            return nullptr;
        }

        if (surface) {
            *surface = scene_surface->surface;
        }
        return node;
    }

    wlr_surface* Scene::surface_at(double lx, double ly, double* sx, double* sy) const {
        wlr_surface* surface = nullptr;
        if (surface_node_at(lx, ly, sx, sy, &surface)) {
            return surface;
        }
        return nullptr;
    }

    Window* Scene::window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) const {
        wlr_scene_node* node = surface_node_at(lx, ly, sx, sy, surface);
        if (!node) {
            return nullptr;
        }

        wlr_scene_tree* tree = node->parent;
        while (tree && !tree->node.data) {
            tree = tree->node.parent;
        }

        return tree ? static_cast<Window*>(tree->node.data) : nullptr;
    }

}
