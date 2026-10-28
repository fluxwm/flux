#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/popup.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <fcntl.h>
#include <ranges>
#include <unistd.h>
#include <vector>

namespace flux {

    Compositor::Compositor() = default;

    Compositor::~Compositor() {
        if (m_display) {
            wl_display_destroy_clients(m_display);
        }

        m_config.reset();

        m_listeners.clear();

        m_popups.clear();
        m_layers.clear();
        m_focus_stack.clear();
        m_windows.clear();
        m_input.reset();
        m_outputs.clear();

        if (m_scene) {
            wlr_scene_node_destroy(&m_scene->tree.node);
        }
        if (m_output_layout) {
            wlr_output_layout_destroy(m_output_layout);
        }
        if (m_allocator) {
            wlr_allocator_destroy(m_allocator);
        }
        if (m_renderer) {
            wlr_renderer_destroy(m_renderer);
        }
        if (m_backend) {
            wlr_backend_destroy(m_backend);
        }
        if (m_display) {
            wl_display_destroy(m_display);
        }
    }

    bool Compositor::init(std::optional<std::filesystem::path> config_path) {
        signal(SIGCHLD, SIG_IGN);

        m_display = wl_display_create();
        if (!m_display) {
            wlr_log(WLR_ERROR, "failed to create wl_display");
            return false;
        }

        wl_event_loop* event_loop = wl_display_get_event_loop(m_display);
        auto on_signal = [](int, void* data) {
            static_cast<Compositor*>(data)->quit();
            return 0;
        };
        for (const int sig : {SIGINT, SIGTERM}) {
            wl_event_loop_add_signal(event_loop, sig, on_signal, this);
        }

        m_config = std::make_unique<Config>(event_loop, config_path);

        m_backend = wlr_backend_autocreate(event_loop, &m_session);
        if (!m_backend) {
            wlr_log(WLR_ERROR, "failed to create wlr_backend");
            return false;
        }

        m_renderer = wlr_renderer_autocreate(m_backend);
        if (!m_renderer) {
            wlr_log(WLR_ERROR, "failed to create wlr_renderer");
            return false;
        }

        wlr_renderer_init_wl_display(m_renderer, m_display);

        m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
        if (!m_allocator) {
            wlr_log(WLR_ERROR, "failed to create wlr_allocator");
            return false;
        }

        wlr_compositor_create(m_display, 5, m_renderer);
        wlr_subcompositor_create(m_display);
        wlr_data_device_manager_create(m_display);
        wlr_ext_data_control_manager_v1_create(m_display, 1);
        wlr_data_control_manager_v1_create(m_display);
        wlr_ext_image_copy_capture_manager_v1_create(m_display, 1);
        wlr_ext_output_image_capture_source_manager_v1_create(m_display, 1);
        wlr_screencopy_manager_v1_create(m_display);
        wlr_export_dmabuf_manager_v1_create(m_display);

        m_output_layout = wlr_output_layout_create(m_display);
        wlr_xdg_output_manager_v1_create(m_display, m_output_layout);

        m_listeners.listen(&m_backend->events.new_output, this, &Compositor::on_new_output);

        m_scene = wlr_scene_create();
        m_scene_background = wlr_scene_tree_create(&m_scene->tree);
        m_scene_bottom = wlr_scene_tree_create(&m_scene->tree);
        m_scene_windows = wlr_scene_tree_create(&m_scene->tree);
        m_scene_top = wlr_scene_tree_create(&m_scene->tree);
        m_scene_fullscreen = wlr_scene_tree_create(&m_scene->tree);
        m_scene_overlay = wlr_scene_tree_create(&m_scene->tree);
        m_scene_layout = wlr_scene_attach_output_layout(m_scene, m_output_layout);

        wlr_scene_set_gamma_control_manager_v1(m_scene, wlr_gamma_control_manager_v1_create(m_display));

        m_xdg_shell = wlr_xdg_shell_create(m_display, 7);
        m_listeners.listen(&m_xdg_shell->events.new_toplevel, this, &Compositor::on_new_window);
        m_listeners.listen(&m_xdg_shell->events.new_popup, this, &Compositor::create_popup);

        m_layer_shell = wlr_layer_shell_v1_create(m_display, 5);
        m_listeners.listen(&m_layer_shell->events.new_surface, this, &Compositor::on_new_layer);

        m_input = std::make_unique<Input>(*this);

        const char* socket = wl_display_add_socket_auto(m_display);
        if (!socket) {
            return false;
        }

        if (!wlr_backend_start(m_backend)) {
            return false;
        }

        setenv("WAYLAND_DISPLAY", socket, 1);
        wlr_log(WLR_INFO, "ready on Wayland socket: %s", socket);
        return true;
    }

    void Compositor::run() {
        if (m_config) {
            for (const auto& action : m_config->autostart()) {
                execute_action(action);
            }
        }

        wl_display_run(m_display);
    }

    void Compositor::quit() noexcept {
        wl_display_terminate(m_display);
    }

    bool Compositor::handle_keybinding(uint32_t modifiers, xkb_keysym_t sym) {
        if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            wlr_session_change_vt(m_session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            return true;
        }

        if (exclusive_layer()) {
            return false;
        }

        if (!m_config) {
            return false;
        }

        constexpr uint32_t clean_mask = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;
        const uint32_t clean_mods = modifiers & clean_mask;
        const xkb_keysym_t lower_sym = xkb_keysym_to_lower(sym);

        for (const auto& bind : m_config->keybinds()) {
            if (bind.modifiers == clean_mods && xkb_keysym_to_lower(bind.keysym) == lower_sym) {
                for (const auto& action : bind.actions) {
                    execute_action(action);
                }
                return true;
            }
        }
        return false;
    }

    void Compositor::execute_action(const Action& action) {
        std::visit(
            [this](const auto& act) {
                using T = std::decay_t<decltype(act)>;
                if constexpr (std::is_same_v<T, action::Quit>) {
                    quit();
                } else if constexpr (std::is_same_v<T, action::CloseWindow>) {
                    if (!m_focus_stack.empty()) {
                        m_focus_stack.front()->close();
                    }
                } else if constexpr (std::is_same_v<T, action::CycleWindow>) {
                    if (m_focus_stack.size() >= 2) {
                        Window* next_window = m_focus_stack[1];
                        focus_window(next_window);
                    }
                } else if constexpr (std::is_same_v<T, action::MaximizeWindow>) {
                    if (!m_focus_stack.empty()) {
                        m_focus_stack.front()->toggle_maximize();
                    }
                } else if constexpr (std::is_same_v<T, action::FullscreenWindow>) {
                    if (!m_focus_stack.empty()) {
                        m_focus_stack.front()->toggle_fullscreen();
                    }
                } else if constexpr (std::is_same_v<T, action::Spawn>) {
                    spawn(act);
                }
            },
            action);
    }

    void Compositor::spawn(const action::Spawn& action) {
        std::vector<const char*> argv;
        argv.reserve(action.args.size() + 2);
        argv.push_back(action.program.c_str());
        for (const auto& arg : action.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        const pid_t pid = fork();
        if (pid < 0) {
            wlr_log(WLR_ERROR, "failed to fork process '%s'", action.program.c_str());
            return;
        }

        if (pid == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);

            sigset_t set;
            sigemptyset(&set);
            sigprocmask(SIG_SETMASK, &set, nullptr);

            const int dev_null = open("/dev/null", O_RDWR | O_CLOEXEC);
            if (dev_null >= 0) {
                dup2(dev_null, STDIN_FILENO);
                dup2(dev_null, STDOUT_FILENO);
                dup2(dev_null, STDERR_FILENO);
            }

            closefrom(3);

            execvp(action.program.c_str(), const_cast<char* const*>(argv.data()));
            _exit(1);
        }
    }

    void Compositor::focus_surface(wlr_surface* surface) {
        if (!m_input) {
            return;
        }

        wlr_surface* prev_surface = m_input->seat().focused_surface();
        if (prev_surface == surface) {
            return;
        }
        if (prev_surface) {
            wlr_surface* prev_root = root_popup_parent(prev_surface);
            if (wlr_xdg_toplevel* prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_root)) {
                wlr_xdg_toplevel_set_activated(prev_toplevel, false);
            }
        }
        m_input->seat().notify_keyboard_enter(surface);
    }

    void Compositor::focus_window(Window* window) {
        if (exclusive_layer()) {
            return;
        }

        if (!window) {
            focus_surface(nullptr);
            return;
        }

        for (Window* w : m_focus_stack) {
            if (w != window && w->is_fullscreen() && window_output(w) == window_output(window)) {
                wlr_scene_node_reparent(&w->scene_tree()->node, m_scene_windows);
            }
        }

        if (window->is_fullscreen()) {
            wlr_scene_node_reparent(&window->scene_tree()->node, m_scene_fullscreen);
        }

        window->raise_to_top();
        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);
        window->set_activated(true);
        focus_surface(window->surface());
    }

    void Compositor::begin_move(Window* window) {
        if (m_input) {
            m_input->cursor().begin_move(window);
        }
    }

    void Compositor::begin_resize(Window* window, uint32_t edges) {
        if (m_input) {
            m_input->cursor().begin_resize(window, edges);
        }
    }

    wlr_scene_tree* Compositor::layer_tree(zwlr_layer_shell_v1_layer layer) const noexcept {
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

    Output* Compositor::output_from_wlr(wlr_output* wlr_out) const noexcept {
        if (!wlr_out) {
            return nullptr;
        }
        auto it = std::ranges::find_if(m_outputs, [wlr_out](const auto& o) { return o->raw() == wlr_out; });
        return it != m_outputs.end() ? it->get() : nullptr;
    }

    Output* Compositor::focused_output() const noexcept {
        if (!m_output_layout || !m_input) {
            return m_outputs.empty() ? nullptr : m_outputs.front().get();
        }
        wlr_output* wlr_out =
            wlr_output_layout_output_at(m_output_layout, m_input->cursor().x(), m_input->cursor().y());
        if (!wlr_out) {
            wlr_out = wlr_output_layout_get_center_output(m_output_layout);
        }
        Output* out = output_from_wlr(wlr_out);
        return out ? out : (m_outputs.empty() ? nullptr : m_outputs.front().get());
    }

    Output* Compositor::window_output(const Window* window) const noexcept {
        if (!window || !m_output_layout || !window->surface() || !window->surface()->mapped) {
            return focused_output();
        }

        const double center_x = window->x() + (window->width() / 2.0);
        const double center_y = window->y() + (window->height() / 2.0);

        wlr_output* wlr_out = wlr_output_layout_output_at(m_output_layout, center_x, center_y);
        Output* out = output_from_wlr(wlr_out);
        return out ? out : focused_output();
    }

    void Compositor::center_window(Window* window) {
        if (!window) {
            return;
        }

        Output* output = focused_output();
        if (!output) {
            return;
        }

        const wlr_box& box = output->usable_area();
        const int center_x = box.x + std::max(0, (box.width - window->width()) / 2);
        const int center_y = box.y + std::max(0, (box.height - window->height()) / 2);

        window->set_position(center_x, center_y);
    }

    void Compositor::map_window(Window* window) {
        if (!window->is_fullscreen() && !window->is_maximized()) {
            center_window(window);
        }
        if (!std::ranges::contains(m_focus_stack, window)) {
            m_focus_stack.insert(m_focus_stack.begin(), window);
        }
        focus_window(window);
    }

    void Compositor::unmap_window(Window* window) {
        if (m_input && window == m_input->cursor().grabbed_window()) {
            m_input->cursor().reset_mode();
        }
        std::erase(m_focus_stack, window);
        refocus();
    }

    Layer* Compositor::exclusive_layer() const noexcept {
        constexpr std::array layers = {
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        };

        for (auto layer : layers) {
            for (const auto& ls : std::views::reverse(m_layers)) {
                auto* raw = ls->raw();
                if (raw && raw->surface->mapped && raw->current.layer == layer &&
                    raw->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                    return ls.get();
                }
            }
        }

        return nullptr;
    }

    void Compositor::refocus() {
        if (auto* excl = exclusive_layer()) {
            focus_layer(excl);
            return;
        }

        wlr_surface* focused = m_input ? m_input->seat().focused_surface() : nullptr;
        if (focused) {
            wlr_surface* root = root_popup_parent(focused);
            if (auto* ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
                ls && ls->surface->mapped &&
                ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
                return;
            }
        }

        focus_window(m_focus_stack.empty() ? nullptr : m_focus_stack.front());
    }

    void Compositor::remove_window(Window* window) {
        unmap_window(window);
        erase_unique_ptr(m_windows, window);
    }

    void Compositor::remove_popup(Popup* popup) {
        erase_unique_ptr(m_popups, popup);
    }

    void Compositor::remove_output(Output* output) {
        const bool is_nested = wlr_output_is_wl(output->raw()) || wlr_output_is_x11(output->raw());

        std::vector<wlr_layer_surface_v1*> to_destroy;
        for (const auto& ls : m_layers) {
            if (ls->raw() && ls->raw()->output == output->raw()) {
                to_destroy.push_back(ls->raw());
            }
        }
        erase_unique_ptr(m_outputs, output);
        for (auto* raw : to_destroy) {
            wlr_layer_surface_v1_destroy(raw);
        }

        if (is_nested && m_outputs.empty()) {
            quit();
        }
    }

    void Compositor::remove_layer(Layer* layer) {
        if (!layer) {
            return;
        }

        Output* output = layer->output();
        erase_unique_ptr(m_layers, layer);

        if (output && output_from_wlr(output->raw())) {
            arrange_layers(output);
        }
        refocus();
    }

    void Compositor::on_new_output(wlr_output* output) {
        m_outputs.push_back(std::make_unique<Output>(*this, output));
    }

    void Compositor::on_new_window(wlr_xdg_toplevel* toplevel) {
        m_windows.push_back(std::make_unique<Window>(*this, toplevel));
    }

    void Compositor::create_popup(wlr_xdg_popup* popup) {
        if (!popup || !popup->parent) {
            return;
        }
        m_popups.push_back(std::make_unique<Popup>(*this, popup));
    }

    void Compositor::on_new_layer(wlr_layer_surface_v1* layer_surface) {
        if (!layer_surface->output) {
            Output* output = focused_output();
            if (!output) {
                wlr_log(WLR_ERROR, "no output available for layer surface");
                wlr_layer_surface_v1_destroy(layer_surface);
                return;
            }
            layer_surface->output = output->raw();
        }

        auto ls = std::make_unique<Layer>(*this, layer_surface);
        if (!ls->raw()) {
            return;
        }
        m_layers.push_back(std::move(ls));
    }

    void Compositor::arrange_layers(Output* output) {
        if (!output || !output->raw()->enabled) {
            return;
        }

        wlr_box output_box{};
        wlr_output_layout_get_box(m_output_layout, output->raw(), &output_box);

        wlr_box full_area = output_box;
        wlr_box usable_area = output_box;

        constexpr std::array layers = {
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP,
            ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
            ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        };

        auto configure = [&](bool exclusive) {
            for (auto layer : layers) {
                for (const auto& ls : m_layers) {
                    if (ls->output() == output && ls->scene_layer() && ls->raw()->initialized &&
                        ls->raw()->current.layer == layer && (ls->raw()->current.exclusive_zone > 0) == exclusive) {
                        wlr_scene_layer_surface_v1_configure(ls->scene_layer(), &full_area, &usable_area);
                    }
                }
            }
        };

        configure(true);
        output->set_usable_area(usable_area);
        configure(false);

        for (const auto& win : m_windows) {
            if (window_output(win.get()) == output) {
                if (win->is_fullscreen()) {
                    win->set_pending_position(full_area.x, full_area.y);
                    win->set_size(full_area.width, full_area.height);
                } else if (win->is_maximized()) {
                    win->set_pending_position(usable_area.x, usable_area.y);
                    win->set_size(usable_area.width, usable_area.height);
                }
            }
        }
    }

    void Compositor::focus_layer(Layer* layer) {
        if (!layer || !layer->raw()->surface->mapped) {
            return;
        }
        if (layer->scene_layer()) {
            wlr_scene_node_raise_to_top(&layer->scene_layer()->tree->node);
        }
        focus_surface(layer->raw()->surface);
    }

    Window* Compositor::window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) {
        wlr_scene_node* node = wlr_scene_node_at(&m_scene->tree.node, lx, ly, sx, sy);
        if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
            return nullptr;
        }
        wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
        if (!scene_surface) {
            return nullptr;
        }
        *surface = scene_surface->surface;

        wlr_scene_tree* tree = node->parent;
        while (tree && !tree->node.data) {
            tree = tree->node.parent;
        }
        return tree ? static_cast<Window*>(tree->node.data) : nullptr;
    }

}
