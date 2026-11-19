#include "flux/scene.hpp"
#include "flux/output.hpp"
#include "flux/window.hpp"

namespace flux {

    void Border::init(wlr_scene_tree* parent) noexcept {
        static constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (auto& rect : m_rects) {
            rect = wlr_scene_rect_create(parent, 0, 0, transparent);
            wlr_scene_node_set_enabled(&rect->node, false);
        }
    }

    void Border::update(int w, int h, int bw, const std::array<float, 4>& color, float opacity) noexcept {
        if (!m_rects[0]) {
            return;
        }
        const std::array<float, 4> c{
            color[0] * opacity,
            color[1] * opacity,
            color[2] * opacity,
            color[3] * opacity,
        };

        wlr_scene_node_set_position(&m_rects[0]->node, -bw, -bw);
        wlr_scene_rect_set_size(m_rects[0], w + (2 * bw), bw);

        wlr_scene_node_set_position(&m_rects[1]->node, -bw, h);
        wlr_scene_rect_set_size(m_rects[1], w + (2 * bw), bw);

        wlr_scene_node_set_position(&m_rects[2]->node, -bw, 0);
        wlr_scene_rect_set_size(m_rects[2], bw, h);

        wlr_scene_node_set_position(&m_rects[3]->node, w, 0);
        wlr_scene_rect_set_size(m_rects[3], bw, h);

        for (auto* rect : m_rects) {
            wlr_scene_rect_set_color(rect, c.data());
            wlr_scene_node_set_enabled(&rect->node, true);
        }
    }

    void Border::hide() noexcept {
        for (auto* rect : m_rects) {
            if (rect) {
                wlr_scene_node_set_enabled(&rect->node, false);
            }
        }
    }

    Scene::Scene(wl_display* display, wlr_output_layout* output_layout) {
        m_scene = wlr_scene_create();
        m_scene_background = wlr_scene_tree_create(&m_scene->tree);
        m_scene_bottom = wlr_scene_tree_create(&m_scene->tree);
        m_scene_windows = wlr_scene_tree_create(&m_scene->tree);
        m_scene_top = wlr_scene_tree_create(&m_scene->tree);
        m_scene_fullscreen = wlr_scene_tree_create(&m_scene->tree);
        m_scene_window_popups = wlr_scene_tree_create(&m_scene->tree);
        m_scene_overlay = wlr_scene_tree_create(&m_scene->tree);
        m_scene_popups = wlr_scene_tree_create(&m_scene->tree);
        m_scene_drag_icons = wlr_scene_tree_create(&m_scene->tree);
        m_scene_lock = wlr_scene_tree_create(&m_scene->tree);
        m_scene_lock_background = wlr_scene_tree_create(m_scene_lock);
        m_scene_lock_surfaces = wlr_scene_tree_create(m_scene_lock);
        wlr_scene_node_set_enabled(&m_scene_lock->node, false);
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

    void Scene::set_session_lock_active(bool active) {
        wlr_scene_node_set_enabled(&m_scene_lock->node, active);
    }

    void Scene::update_lock_background(std::span<const std::unique_ptr<Output>> outputs) {
        wlr_scene_node* child = nullptr;
        wlr_scene_node* next = nullptr;
        wl_list_for_each_safe(child, next, &m_scene_lock_background->children, link) {
            wlr_scene_node_destroy(child);
        }
        static constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (const auto& out : outputs) {
            if (!out->is_enabled()) {
                continue;
            }
            const wlr_box box = out->layout_box();
            if (box.width <= 0 || box.height <= 0) {
                continue;
            }
            auto* rect = wlr_scene_rect_create(m_scene_lock_background, box.width, box.height, black);
            wlr_scene_node_set_position(&rect->node, box.x, box.y);
        }
    }

    wlr_scene_node* Scene::interactive_node_at(double lx, double ly, double* sx, double* sy) const {
        if (m_scene_lock->node.enabled) {
            return wlr_scene_node_at(&m_scene_lock->node, lx, ly, sx, sy);
        }

        wlr_scene_node* node = nullptr;
        wl_list_for_each_reverse(node, &m_scene->tree.children, link) {
            if (node == &m_scene_drag_icons->node) {
                continue;
            }
            auto find_hit = [&](auto& self, wlr_scene_node* n) -> wlr_scene_node* {
                wlr_scene_node* hit = wlr_scene_node_at(n, lx, ly, sx, sy);
                if (hit && hit->type == WLR_SCENE_NODE_RECT && hit->parent && !hit->parent->node.data) {
                    hit->parent->node.enabled = false;
                    wlr_scene_node* next = self(self, n);
                    hit->parent->node.enabled = true;
                    return next;
                }
                return hit;
            };
            if (wlr_scene_node* hit = find_hit(find_hit, node)) {
                return hit;
            }
        }
        return nullptr;
    }

    wlr_surface* Scene::surface_at(double lx, double ly, double* sx, double* sy) const {
        wlr_scene_node* node = interactive_node_at(lx, ly, sx, sy);
        if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
            return nullptr;
        }
        wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
        return scene_surface ? scene_surface->surface : nullptr;
    }

    Window* Scene::window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) const {
        wlr_scene_node* node = interactive_node_at(lx, ly, sx, sy);
        if (!node) {
            return nullptr;
        }
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
            wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
            if (!scene_surface) {
                return nullptr;
            }
            if (surface) {
                *surface = scene_surface->surface;
            }
        } else if (node->type != WLR_SCENE_NODE_RECT) {
            return nullptr;
        }

        wlr_scene_tree* tree = node->parent;
        while (tree && !tree->node.data) {
            tree = tree->node.parent;
        }

        auto* window = tree ? static_cast<Window*>(tree->node.data) : nullptr;
        if (window && surface && !*surface) {
            *surface = window->surface();
        }
        return window;
    }

}
