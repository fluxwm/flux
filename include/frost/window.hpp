#pragma once

#include "frost/animation.hpp"
#include "frost/scene.hpp"
#include "frost/wlroots.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace frost {

    class Compositor;
    class Workspace;
    class Output;

    class Window {
    public:
        virtual ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        [[nodiscard]] static Window* try_from(wlr_xdg_toplevel* toplevel) noexcept {
            if (!toplevel || !toplevel->base || !toplevel->base->data) {
                return nullptr;
            }
            auto* tree = static_cast<wlr_scene_tree*>(toplevel->base->data);
            return static_cast<Window*>(tree->node.data);
        }

        [[nodiscard]] static Window* try_from(wlr_xwayland_surface* xsurface) noexcept {
            if (!xsurface || xsurface->override_redirect || !xsurface->data) {
                return nullptr;
            }
            auto* tree = static_cast<wlr_scene_tree*>(xsurface->data);
            return static_cast<Window*>(tree->node.data);
        }

        [[nodiscard]] static Window* try_from(wlr_surface* surface) noexcept {
            if (!surface) {
                return nullptr;
            }
            wlr_surface* root = wlr_surface_get_root_surface(surface);
            if (!root) {
                return nullptr;
            }
            if (auto* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root)) {
                return try_from(toplevel);
            }
            if (auto* xsurface = wlr_xwayland_surface_try_from_wlr_surface(root)) {
                return try_from(xsurface);
            }
            return nullptr;
        }

        [[nodiscard]] wlr_scene_tree* scene_tree() const noexcept { return m_scene_tree; }
        [[nodiscard]] virtual wlr_surface* surface() const noexcept = 0;
        [[nodiscard]] virtual wlr_xwayland_surface* xwayland_surface() const noexcept { return nullptr; }
        [[nodiscard]] std::string_view title() const noexcept { return m_title; }
        [[nodiscard]] std::string_view app_id() const noexcept { return m_app_id; }
        [[nodiscard]] bool is_maximized() const noexcept { return m_maximized; }
        [[nodiscard]] bool is_fullscreen() const noexcept { return m_fullscreen; }
        [[nodiscard]] bool is_floating() const noexcept { return !m_fullscreen && !m_maximized && m_floating; }
        [[nodiscard]] bool is_tiled() const noexcept { return !m_fullscreen && !m_maximized && !m_floating; }
        [[nodiscard]] bool is_placed() const noexcept { return m_placed; }
        [[nodiscard]] virtual bool is_pending_center() const noexcept { return false; }
        [[nodiscard]] virtual bool is_pending_move() const noexcept { return false; }
        [[nodiscard]] bool is_mapped() const noexcept {
            const auto* s = surface();
            return s && s->mapped;
        }
        [[nodiscard]] wlr_box geometry_box() const noexcept;
        [[nodiscard]] bool is_animating() const noexcept { return m_animation.is_active(); }
        [[nodiscard]] bool is_activated() const noexcept { return m_activated; }
        [[nodiscard]] bool is_slide_hidden() const noexcept { return m_slide_hidden; }
        [[nodiscard]] bool has_visible_borders() const noexcept;
        [[nodiscard]] float tile_weight() const noexcept { return m_tile_weight; }
        void set_tile_weight(float weight) noexcept { m_tile_weight = weight; }
        virtual void set_tiled_resizing(bool /*resizing*/) noexcept {}
        [[nodiscard]] Workspace* workspace() const noexcept { return m_workspace; }
        [[nodiscard]] uint32_t workspace_id() const noexcept { return m_workspace_id; }
        [[nodiscard]] Output* output() const noexcept;
        void set_output(Output* output) noexcept;
        void set_placed(bool placed) noexcept {
            m_placed = placed;
            if (!placed) {
                m_animation.set_has_initial_position(false);
            }
        }

        void snap_animation() noexcept;
        bool step_animation(TimePoint now) noexcept { return m_animation.step(now); }

        void set_activated(bool activated) noexcept;
        void update_borders() noexcept;
        void set_slide_clip(const wlr_box& output_box, int ws_offset_x) noexcept;
        void clear_slide_clip() noexcept;
        void set_workspace(Workspace* ws) noexcept;
        void reparent_to_workspace() noexcept;
        void set_position(int x, int y) noexcept;
        void move_scene_tree(int x, int y) noexcept { m_animation.move_scene_tree(x, y); }
        void set_geometry(const wlr_box& box) noexcept;
        void center(const wlr_box& area) noexcept;
        void toggle_maximize();
        void set_fullscreen(bool fullscreen, Output* target_output = nullptr);
        void toggle_fullscreen() { set_fullscreen(!m_fullscreen); }
        void set_floating(bool floating);
        void toggle_floating();
        void set_floating_in_grab(bool floating, const wlr_box& box) noexcept;
        [[nodiscard]] Compositor& compositor() const noexcept { return m_compositor; }
        [[nodiscard]] bool is_grabbed() const noexcept;
        [[nodiscard]] bool is_resizing() const noexcept;

        void prepare_grab() noexcept;
        void begin_resize(uint32_t edges, const wlr_box& grab_box) noexcept;
        virtual void apply_resize(int w, int h) noexcept = 0;
        void finish_resize() noexcept;
        virtual void set_decoration(wlr_xdg_toplevel_decoration_v1* /*decoration*/) {}
        virtual void close() noexcept = 0;

    protected:
        static constexpr uint32_t tiled_edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;

        Window(Compositor& compositor, const char* title, const char* app_id);

        [[nodiscard]] wlr_scene_tree* initial_parent_tree() const noexcept;
        void attach_scene_tree(wlr_scene_tree* tree) noexcept;
        static void clamp_size(int& w, int& h, int min_w, int min_h, int max_w, int max_h) noexcept;
        void on_map();
        void on_unmap();
        void finish_commit(int old_x, int old_y);
        void check_center() noexcept;
        void on_destroy();
        void on_set_parent();
        void update_title(const char* title);
        void update_app_id(const char* app_id);

        [[nodiscard]] virtual std::pair<int, int> preferred_size() const noexcept = 0;
        [[nodiscard]] virtual std::pair<int, int> geometry_offset() const noexcept { return {0, 0}; }
        [[nodiscard]] virtual wlr_scene_node* clip_target() const noexcept {
            return m_scene_tree ? &m_scene_tree->node : nullptr;
        }
        virtual void apply_activated(bool activated) noexcept = 0;
        virtual void apply_fullscreen(bool fullscreen) noexcept = 0;
        virtual void apply_maximized(bool /*maximized*/) noexcept {}
        virtual void apply_tiled(uint32_t /*edges*/) noexcept {}
        virtual void schedule_configure() noexcept {}
        virtual void configure_position(int /*x*/, int /*y*/) noexcept {}
        [[nodiscard]] virtual bool configure_geometry(const wlr_box& box) noexcept = 0;
        virtual void start_resizing() noexcept {}
        virtual void end_resizing() noexcept { m_resize_edges = WLR_EDGE_NONE; }
        virtual void clear_pending_serials() noexcept {}
        virtual void clear_pending_move() noexcept {}
        virtual void clear_pending_center() noexcept {}
        virtual void request_default_floating_size() noexcept {}
        [[nodiscard]] virtual Window* query_parent() const noexcept = 0;
        [[nodiscard]] virtual bool wants_floating() const noexcept = 0;

        Compositor& m_compositor;
        WindowAnimation m_animation;
        wlr_scene_tree* m_scene_tree = nullptr;
        bool m_maximized = false;
        bool m_fullscreen = false;
        bool m_floating = false;
        bool m_floating_override = false;
        bool m_placed = false;
        bool m_resizing = false;
        uint32_t m_resize_edges = WLR_EDGE_NONE;
        wlr_box m_grab_box{};
        Workspace* m_workspace = nullptr;

    private:
        friend class WindowAnimation;

        void restore_floating_box() noexcept;
        void save_floating_box() noexcept;
        void reset_placement_state() noexcept;
        [[nodiscard]] wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state() const noexcept;
        void update_foreign_toplevel() noexcept;

        std::optional<wlr_box> m_slide_clip;
        bool m_slide_hidden = false;
        bool m_activated = false;
        float m_tile_weight = 1.0f;
        wlr_box m_floating_box{};
        int m_committed_width = 0;
        int m_committed_height = 0;
        int m_border_width = 0;
        int m_border_height = 0;
        uint32_t m_workspace_id = 1;
        std::string m_title;
        std::string m_app_id;
        wlr_ext_foreign_toplevel_handle_v1* m_ext_toplevel_handle = nullptr;
        Border m_border;
    };

}
