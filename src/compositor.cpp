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

        m_listeners.clear();

        m_popups.clear();
        m_layers.clear();
        m_focus_stack.clear();
        m_windows.clear();
        m_input.reset();
        m_config.reset();
        m_outputs.clear();

        m_scene.reset();

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

        wlr_renderer_init_wl_shm(m_renderer, m_display);

        m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
        if (!m_allocator) {
            wlr_log(WLR_ERROR, "failed to create wlr_allocator");
            return false;
        }

        wlr_compositor_create(m_display, 6, m_renderer);
        wlr_subcompositor_create(m_display);
        wlr_data_device_manager_create(m_display);
        wlr_ext_data_control_manager_v1_create(m_display, 1);
        wlr_data_control_manager_v1_create(m_display);
        wlr_ext_image_copy_capture_manager_v1_create(m_display, 1);
        wlr_ext_output_image_capture_source_manager_v1_create(m_display, 1);
        wlr_screencopy_manager_v1_create(m_display);
        wlr_export_dmabuf_manager_v1_create(m_display);
        wlr_viewporter_create(m_display);
        wlr_fractional_scale_manager_v1_create(m_display, 1);
        wlr_presentation_create(m_display, m_backend, 2);
        wlr_single_pixel_buffer_manager_v1_create(m_display);
        m_foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(m_display, 1);

        m_output_layout = wlr_output_layout_create(m_display);
        wlr_xdg_output_manager_v1_create(m_display, m_output_layout);

        m_listeners.listen(&m_backend->events.new_output, this, &Compositor::on_new_output);

        m_scene = std::make_unique<Scene>(m_display, m_output_layout);

        if (wlr_renderer_get_drm_fd(m_renderer) >= 0 &&
            wlr_renderer_get_texture_formats(m_renderer, WLR_BUFFER_CAP_DMABUF) != nullptr) {
            if (auto* linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(m_display, 5, m_renderer)) {
                m_scene->set_linux_dmabuf_v1(linux_dmabuf);
            }
        }

        auto* xdg_shell = wlr_xdg_shell_create(m_display, 7);
        m_listeners.listen(&xdg_shell->events.new_toplevel, this, &Compositor::on_new_window);
        m_listeners.listen(&xdg_shell->events.new_popup, this, &Compositor::create_popup);

        auto* layer_shell = wlr_layer_shell_v1_create(m_display, 5);
        m_listeners.listen(&layer_shell->events.new_surface, this, &Compositor::on_new_layer);

        auto* xdg_activation = wlr_xdg_activation_v1_create(m_display);
        m_listeners.listen(&xdg_activation->events.request_activate, this, &Compositor::on_request_activate);

        m_input = std::make_unique<Input>(*this);
        m_config->set_on_reload([this] { m_input->apply_config(); });

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
        for (const auto& action : m_config->autostart()) {
            execute_action(action);
        }

        wl_display_run(m_display);
    }

    void Compositor::quit() noexcept {
        wl_display_terminate(m_display);
    }

    bool Compositor::handle_keybinding(uint32_t modifiers, xkb_keysym_t sym) {
        if (m_session && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            wlr_session_change_vt(m_session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            return true;
        }

        if (exclusive_layer()) {
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
                        std::ranges::rotate(m_focus_stack, m_focus_stack.begin() + 1);
                        focus_window(m_focus_stack.front());
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
            m_input->cursor().update_focus();
            return;
        }

        for (Window* w : m_focus_stack) {
            if (w != window && w->is_fullscreen() && window_output(w) == window_output(window)) {
                wlr_scene_node_reparent(&w->scene_tree()->node, m_scene->windows());
            }
        }

        if (window->is_fullscreen()) {
            wlr_scene_node_reparent(&window->scene_tree()->node, m_scene->fullscreen());
        }

        window->raise_to_top();
        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);
        window->set_activated(true);
        focus_surface(window->surface());
        m_input->cursor().update_focus();
    }

    void Compositor::begin_move(Window* window) {
        m_input->cursor().begin_move(window);
    }

    void Compositor::begin_resize(Window* window, uint32_t edges) {
        m_input->cursor().begin_resize(window, edges);
    }

    Output* Compositor::output_from_wlr(wlr_output* wlr_out) const noexcept {
        if (!wlr_out) {
            return nullptr;
        }
        auto it = std::ranges::find_if(m_outputs, [wlr_out](const auto& o) { return o->matches(wlr_out); });
        return it != m_outputs.end() ? it->get() : nullptr;
    }

    Output* Compositor::focused_output() const noexcept {
        wlr_output* wlr_out =
            wlr_output_layout_output_at(m_output_layout, m_input->cursor().x(), m_input->cursor().y());
        if (!wlr_out) {
            wlr_out = wlr_output_layout_get_center_output(m_output_layout);
        }
        Output* out = output_from_wlr(wlr_out);
        return out ? out : (m_outputs.empty() ? nullptr : m_outputs.front().get());
    }

    Output* Compositor::window_output(const Window* window) const noexcept {
        if (!window || !window->is_mapped()) {
            return focused_output();
        }

        wlr_output* wlr_out = wlr_output_layout_output_at(m_output_layout, window->center_x(), window->center_y());
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

        const auto centered = center_box(output->usable_area(), window->width(), window->height());
        window->set_position(centered.x, centered.y);
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
        if (window == m_input->cursor().grabbed_window()) {
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
                if (ls->is_mapped() && ls->layer() == layer && ls->is_exclusive()) {
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

        if (wlr_surface* focused = m_input->seat().focused_surface()) {
            if (auto* layer = Layer::try_from(root_popup_parent(focused));
                layer && layer->is_mapped() && layer->is_focusable()) {
                m_input->cursor().update_focus();
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
        const bool is_nested = output && output->is_nested();

        std::vector<wlr_layer_surface_v1*> to_destroy;
        for (const auto& ls : m_layers) {
            if (ls->output() == output) {
                to_destroy.push_back(ls->raw());
            }
        }
        erase_unique_ptr(m_outputs, output);
        for (auto* raw : to_destroy) {
            wlr_layer_surface_v1_destroy(raw);
        }

        if (is_nested && m_outputs.empty()) {
            quit();
            return;
        }

        for (const auto& remaining : m_outputs) {
            arrange_layers(remaining.get());
        }

        if (Output* target = focused_output()) {
            const wlr_box& usable = target->usable_area();
            const wlr_box full = target->layout_box();

            for (const auto& win : m_windows) {
                if (!wlr_output_layout_output_at(m_output_layout, win->center_x(), win->center_y())) {
                    if (win->is_fullscreen()) {
                        win->set_position(full.x, full.y);
                    } else if (win->is_maximized()) {
                        win->set_position(usable.x, usable.y);
                    } else {
                        center_window(win.get());
                    }
                }
            }
        }
    }

    void Compositor::remove_layer(Layer* layer) {
        if (!layer) {
            return;
        }

        Output* output = layer->output();
        erase_unique_ptr(m_layers, layer);

        if (output) {
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

    void Compositor::on_request_activate(wlr_xdg_activation_v1_request_activate_event* event) {
        if (!event->surface->mapped) {
            return;
        }

        auto it = std::ranges::find_if(m_windows, [event](const auto& w) { return w->matches(event->surface); });
        if (it != m_windows.end()) {
            focus_window(it->get());
        }
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

        m_layers.push_back(std::make_unique<Layer>(*this, layer_surface));
    }

    void Compositor::arrange_layers(Output* output) {
        if (!output || !output->is_enabled()) {
            return;
        }

        wlr_box output_box = output->layout_box();
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
                    if (ls->output() == output && ls->scene_layer() && ls->is_initialized() && ls->layer() == layer &&
                        (ls->exclusive_zone() > 0) == exclusive) {
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
                    win->set_geometry(full_area);
                } else if (win->is_maximized()) {
                    win->set_geometry(usable_area);
                }
            }
        }

        m_input->cursor().update_focus();
    }

    void Compositor::focus_layer(Layer* layer) {
        if (!layer || !layer->is_mapped()) {
            return;
        }
        if (layer->scene_tree()) {
            wlr_scene_node_raise_to_top(&layer->scene_tree()->node);
        }
        focus_surface(layer->surface());
        m_input->cursor().update_focus();
    }

}
