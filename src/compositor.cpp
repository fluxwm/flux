#include "flux/compositor.hpp"
#include "flux/keyboard.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/popup.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <ranges>
#include <string>
#include <unistd.h>
#include <vector>

namespace flux {

    namespace {
        constexpr uint32_t btn_left = 0x110;
        constexpr uint32_t btn_right = 0x111;
    }

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
        m_keyboards.clear();
        m_outputs.clear();

        if (m_scene) {
            wlr_scene_node_destroy(&m_scene->tree.node);
        }
        if (m_cursor_mgr) {
            wlr_xcursor_manager_destroy(m_cursor_mgr);
        }
        if (m_cursor) {
            wlr_cursor_destroy(m_cursor);
        }
        if (m_output_layout) {
            wlr_output_layout_destroy(m_output_layout);
        }
        if (m_seat) {
            wlr_seat_destroy(m_seat);
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

        std::filesystem::path resolved_path;
        if (config_path.has_value()) {
            resolved_path = *config_path;
        } else {
            const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
            if (xdg_config && *xdg_config != '\0') {
                resolved_path = std::filesystem::path(xdg_config) / "flux" / "config.kdl";
            } else {
                const char* home = std::getenv("HOME");
                if (home && *home != '\0') {
                    resolved_path = std::filesystem::path(home) / ".config" / "flux" / "config.kdl";
                }
            }
        }

        if (!resolved_path.empty() && std::filesystem::exists(resolved_path)) {
            auto cfg = Config::load(resolved_path);
            if (cfg) {
                m_config = std::move(*cfg);
            }
        }

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

        m_output_layout = wlr_output_layout_create(m_display);
        if (!m_output_layout) {
            wlr_log(WLR_ERROR, "failed to create wlr_output_layout");
            return false;
        }

        if (!wlr_xdg_output_manager_v1_create(m_display, m_output_layout)) {
            wlr_log(WLR_ERROR, "failed to create wlr_xdg_output_manager_v1");
            return false;
        }

        m_listeners.listen(&m_backend->events.new_output, this, &Compositor::on_new_output);

        m_scene = wlr_scene_create();
        m_scene_background = wlr_scene_tree_create(&m_scene->tree);
        m_scene_bottom = wlr_scene_tree_create(&m_scene->tree);
        m_scene_windows = wlr_scene_tree_create(&m_scene->tree);
        m_scene_top = wlr_scene_tree_create(&m_scene->tree);
        m_scene_overlay = wlr_scene_tree_create(&m_scene->tree);
        m_scene_layout = wlr_scene_attach_output_layout(m_scene, m_output_layout);

        m_xdg_shell = wlr_xdg_shell_create(m_display, 7);
        m_listeners.listen(&m_xdg_shell->events.new_toplevel, this, &Compositor::on_new_window);
        m_listeners.listen(&m_xdg_shell->events.new_popup, this, &Compositor::create_popup);

        m_layer_shell = wlr_layer_shell_v1_create(m_display, 5);
        if (!m_layer_shell) {
            wlr_log(WLR_ERROR, "failed to create wlr_layer_shell_v1");
            return false;
        }
        m_listeners.listen(&m_layer_shell->events.new_surface, this, &Compositor::on_new_layer);

        m_cursor = wlr_cursor_create();
        wlr_cursor_attach_output_layout(m_cursor, m_output_layout);

        m_cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

        m_cursor_state = cursor_state::Passthrough{};

        m_listeners.listen(&m_cursor->events.motion, this, &Compositor::on_cursor_motion);
        m_listeners.listen(&m_cursor->events.motion_absolute, this, &Compositor::on_cursor_motion_absolute);
        m_listeners.listen(&m_cursor->events.button, this, &Compositor::on_cursor_button);
        m_listeners.listen(&m_cursor->events.axis, this, &Compositor::on_cursor_axis);
        m_listeners.listen(&m_cursor->events.frame, this, &Compositor::on_cursor_frame);

        m_listeners.listen(&m_backend->events.new_input, this, &Compositor::on_new_input);

        m_seat = wlr_seat_create(m_display, "seat0");

        m_listeners.listen(&m_seat->events.request_set_cursor, this, &Compositor::on_request_cursor);
        m_listeners.listen(&m_seat->pointer_state.events.focus_change, this, &Compositor::on_pointer_focus_change);
        m_listeners.listen(&m_seat->events.request_set_selection, this, &Compositor::on_request_set_selection);

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
        for (const auto& action : m_config.autostart) {
            execute_action(action);
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

        constexpr uint32_t clean_mask = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;
        const uint32_t clean_mods = modifiers & clean_mask;
        const xkb_keysym_t lower_sym = xkb_keysym_to_lower(sym);

        for (const auto& bind : m_config.keybinds) {
            if (bind.modifiers == clean_mods && xkb_keysym_to_lower(bind.keysym) == lower_sym) {
                execute_action(bind.action);
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

        if (fork() == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);

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
        wlr_surface* prev_surface = m_seat->keyboard_state.focused_surface;
        if (prev_surface == surface) {
            return;
        }
        if (prev_surface) {
            wlr_surface* prev_root = root_popup_parent(prev_surface);
            if (wlr_xdg_toplevel* prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_root)) {
                wlr_xdg_toplevel_set_activated(prev_toplevel, false);
            }
        }
        if (!surface) {
            wlr_seat_keyboard_notify_clear_focus(m_seat);
            return;
        }
        wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_seat);
        if (!keyboard) {
            return;
        }
        wlr_seat_keyboard_notify_enter(m_seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }

    void Compositor::focus_window(Window* window) {
        if (exclusive_layer()) {
            return;
        }

        if (!window) {
            focus_surface(nullptr);
            return;
        }

        window->raise_to_top();
        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);
        window->set_activated(true);
        focus_surface(window->surface());
    }

    void Compositor::begin_move(Window* window) {
        if (m_seat->pointer_state.button_count == 0) {
            return;
        }
        m_cursor_state = cursor_state::Move{
            .window = window,
            .grab_x = m_cursor->x - window->x(),
            .grab_y = m_cursor->y - window->y(),
        };
        wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "grabbing");
    }

    void Compositor::begin_resize(Window* window, uint32_t edges) {
        if (m_seat->pointer_state.button_count == 0) {
            return;
        }
        const wlr_box& geo_box = window->geometry();

        const double border_x = (window->x() + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        const double border_y = (window->y() + geo_box.y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);

        wlr_box geobox = geo_box;
        geobox.x += window->x();
        geobox.y += window->y();

        m_cursor_state = cursor_state::Resize{
            .window = window,
            .grab_x = m_cursor->x - border_x,
            .grab_y = m_cursor->y - border_y,
            .initial_geometry = geobox,
            .resize_edges = edges,
        };

        const char* resize_name = wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges));
        wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, resize_name);
    }

    void Compositor::reset_cursor_mode() {
        if (Window* window = grabbed_window()) {
            window->apply_pending_position();
        }
        m_cursor_state = cursor_state::Passthrough{};
        wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "default");
        process_cursor_motion(0);
    }

    Window* Compositor::grabbed_window() const noexcept {
        return std::visit(
            [](const auto& state) -> Window* {
                using T = std::decay_t<decltype(state)>;
                if constexpr (!std::is_same_v<T, cursor_state::Passthrough>) {
                    return state.window;
                }
                return nullptr;
            },
            m_cursor_state);
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
        if (!m_output_layout || !m_cursor) {
            return m_outputs.empty() ? nullptr : m_outputs.front().get();
        }
        wlr_output* wlr_out = wlr_output_layout_output_at(m_output_layout, m_cursor->x, m_cursor->y);
        if (!wlr_out) {
            wlr_out = wlr_output_layout_get_center_output(m_output_layout);
        }
        Output* out = output_from_wlr(wlr_out);
        return out ? out : (m_outputs.empty() ? nullptr : m_outputs.front().get());
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
        const wlr_box& geo = window->geometry();
        const int width = geo.width > 0 ? geo.width : window->surface()->current.width;
        const int height = geo.height > 0 ? geo.height : window->surface()->current.height;

        const int center_x = box.x + std::max(0, (box.width - width) / 2);
        const int center_y = box.y + std::max(0, (box.height - height) / 2);

        window->set_position(center_x - geo.x, center_y - geo.y);
    }

    void Compositor::map_window(Window* window) {
        center_window(window);
        if (!std::ranges::contains(m_focus_stack, window)) {
            m_focus_stack.insert(m_focus_stack.begin(), window);
        }
        focus_window(window);
    }

    void Compositor::unmap_window(Window* window) {
        if (window == grabbed_window()) {
            reset_cursor_mode();
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

        wlr_surface* focused = m_seat->keyboard_state.focused_surface;
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

    void Compositor::remove_keyboard(Keyboard* keyboard) {
        if (wlr_seat_get_keyboard(m_seat) == keyboard->raw()) {
            auto it = std::ranges::find_if(m_keyboards, [keyboard](const auto& k) { return k.get() != keyboard; });
            wlr_seat_set_keyboard(m_seat, it != m_keyboards.end() ? (*it)->raw() : nullptr);
        }

        erase_unique_ptr(m_keyboards, keyboard);
        uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
        if (!m_keyboards.empty()) {
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        }
        wlr_seat_set_capabilities(m_seat, caps);
    }

    void Compositor::on_new_output(wlr_output* output) {
        m_outputs.push_back(std::make_unique<Output>(*this, output));
    }

    void Compositor::on_new_window(wlr_xdg_toplevel* toplevel) {
        m_windows.push_back(std::make_unique<Window>(*this, toplevel));
    }

    void Compositor::create_popup(wlr_xdg_popup* popup) {
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

    void Compositor::on_cursor_motion(wlr_pointer_motion_event* event) {
        wlr_cursor_move(m_cursor, &event->pointer->base, event->delta_x, event->delta_y);
        process_cursor_motion(event->time_msec);
    }

    void Compositor::on_cursor_motion_absolute(wlr_pointer_motion_absolute_event* event) {
        wlr_cursor_warp_absolute(m_cursor, &event->pointer->base, event->x, event->y);
        process_cursor_motion(event->time_msec);
    }

    void Compositor::on_cursor_button(wlr_pointer_button_event* event) {
        if (std::holds_alternative<cursor_state::Passthrough>(m_cursor_state)) {
            if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
                double sx = 0;
                double sy = 0;
                wlr_surface* surface = nullptr;
                Window* window = window_at(m_cursor->x, m_cursor->y, &surface, &sx, &sy);

                if (auto* excl = exclusive_layer()) {
                    if (surface && root_popup_parent(surface) == excl->raw()->surface) {
                        focus_surface(surface);
                    }
                } else if (window != nullptr) {
                    focus_window(window);
                } else if (surface != nullptr) {
                    wlr_surface* root = root_popup_parent(surface);
                    if (auto* ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
                        ls && ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
                        focus_surface(surface);
                    }
                } else {
                    focus_window(nullptr);
                }

                wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_seat);
                const uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
                const bool mod_down = (modifiers & WLR_MODIFIER_LOGO) != 0;

                if (window != nullptr && mod_down && !exclusive_layer()) {
                    if (event->button == btn_left || event->button == btn_right) {
                        wlr_seat_pointer_clear_focus(m_seat);
                        wlr_seat_pointer_notify_button(m_seat, event->time_msec, event->button, event->state);

                        if (event->button == btn_left) {
                            begin_move(window);
                        } else {
                            const wlr_box& geo = window->geometry();
                            const int width = geo.width > 0 ? geo.width : window->surface()->current.width;
                            const int height = geo.height > 0 ? geo.height : window->surface()->current.height;
                            const double rx = m_cursor->x - (window->x() + geo.x);
                            const double ry = m_cursor->y - (window->y() + geo.y);

                            uint32_t edges = 0;
                            if (rx < width / 3.0) {
                                edges |= WLR_EDGE_LEFT;
                            } else if (rx > 2.0 * width / 3.0) {
                                edges |= WLR_EDGE_RIGHT;
                            }
                            if (ry < height / 3.0) {
                                edges |= WLR_EDGE_TOP;
                            } else if (ry > 2.0 * height / 3.0) {
                                edges |= WLR_EDGE_BOTTOM;
                            }
                            if (edges == 0) {
                                edges = (rx < width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                                        (ry < height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
                            }

                            begin_resize(window, edges);
                        }
                        return;
                    }
                }
            }
            wlr_seat_pointer_notify_button(m_seat, event->time_msec, event->button, event->state);
        } else {
            wlr_seat_pointer_notify_button(m_seat, event->time_msec, event->button, event->state);
            if (m_seat->pointer_state.button_count == 0) {
                reset_cursor_mode();
            }
        }
    }

    void Compositor::on_cursor_axis(wlr_pointer_axis_event* event) {
        wlr_seat_pointer_notify_axis(m_seat, event->time_msec, event->orientation, event->delta, event->delta_discrete,
                                     event->source, event->relative_direction);
    }

    void Compositor::on_cursor_frame() {
        wlr_seat_pointer_notify_frame(m_seat);
    }

    void Compositor::on_new_input(wlr_input_device* device) {
        switch (device->type) {
            case WLR_INPUT_DEVICE_KEYBOARD:
                m_keyboards.push_back(std::make_unique<Keyboard>(*this, device));
                break;
            case WLR_INPUT_DEVICE_POINTER:
                wlr_cursor_attach_input_device(m_cursor, device);
                break;
            default:
                break;
        }
        uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
        if (!m_keyboards.empty()) {
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        }
        wlr_seat_set_capabilities(m_seat, caps);
    }

    void Compositor::on_request_cursor(wlr_seat_pointer_request_set_cursor_event* event) {
        wlr_seat_client* focused_client = m_seat->pointer_state.focused_client;
        if (focused_client == event->seat_client && std::holds_alternative<cursor_state::Passthrough>(m_cursor_state)) {
            wlr_cursor_set_surface(m_cursor, event->surface, event->hotspot_x, event->hotspot_y);
        }
    }

    void Compositor::on_pointer_focus_change(wlr_seat_pointer_focus_change_event* event) {
        if (event->new_surface == nullptr && std::holds_alternative<cursor_state::Passthrough>(m_cursor_state)) {
            wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "default");
        }
    }

    void Compositor::on_request_set_selection(wlr_seat_request_set_selection_event* event) {
        wlr_seat_set_selection(m_seat, event->source, event->serial);
    }

    void Compositor::process_cursor_move(const cursor_state::Move& state) {
        state.window->set_position(m_cursor->x - state.grab_x, m_cursor->y - state.grab_y);
    }

    void Compositor::process_cursor_resize(const cursor_state::Resize& state) {
        const double border_x = m_cursor->x - state.grab_x;
        const double border_y = m_cursor->y - state.grab_y;
        int new_left = state.initial_geometry.x;
        int new_right = state.initial_geometry.x + state.initial_geometry.width;
        int new_top = state.initial_geometry.y;
        int new_bottom = state.initial_geometry.y + state.initial_geometry.height;

        if (state.resize_edges & WLR_EDGE_TOP) {
            new_top = std::min(static_cast<int>(border_y), new_bottom - 1);
        } else if (state.resize_edges & WLR_EDGE_BOTTOM) {
            new_bottom = std::max(static_cast<int>(border_y), new_top + 1);
        }
        if (state.resize_edges & WLR_EDGE_LEFT) {
            new_left = std::min(static_cast<int>(border_x), new_right - 1);
        } else if (state.resize_edges & WLR_EDGE_RIGHT) {
            new_right = std::max(static_cast<int>(border_x), new_left + 1);
        }

        auto clamp_dim = [](int& start, int& end, int min_val, int max_val, bool from_start) {
            const int upper = max_val > 0 ? max_val : std::numeric_limits<int>::max();
            const int size = std::clamp(end - start, min_val, upper);
            if (from_start) {
                start = end - size;
            } else {
                end = start + size;
            }
            return size;
        };

        const auto* toplevel = state.window->raw();
        int new_w = clamp_dim(new_left, new_right, std::max(1, toplevel->current.min_width),
                              toplevel->current.max_width, state.resize_edges & WLR_EDGE_LEFT);
        int new_h = clamp_dim(new_top, new_bottom, std::max(1, toplevel->current.min_height),
                              toplevel->current.max_height, state.resize_edges & WLR_EDGE_TOP);

        const wlr_box& geo_box = state.window->geometry();
        if (state.resize_edges & (WLR_EDGE_TOP | WLR_EDGE_LEFT)) {
            state.window->set_pending_position(new_left - geo_box.x, new_top - geo_box.y);
        }
        state.window->set_size(new_w, new_h);
    }

    void Compositor::process_cursor_motion(uint32_t time_msec) {
        bool handled = std::visit(
            [this](const auto& state) -> bool {
                using T = std::decay_t<decltype(state)>;
                if constexpr (std::is_same_v<T, cursor_state::Move>) {
                    process_cursor_move(state);
                    return true;
                } else if constexpr (std::is_same_v<T, cursor_state::Resize>) {
                    process_cursor_resize(state);
                    return true;
                }
                return false;
            },
            m_cursor_state);

        if (handled) {
            return;
        }

        double sx = 0;
        double sy = 0;
        wlr_surface* surface = nullptr;
        Window* window = window_at(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
        if (!surface) {
            wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "default");
        }
        if (surface) {
            wlr_seat_pointer_notify_enter(m_seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(m_seat, time_msec, sx, sy);
        } else {
            wlr_seat_pointer_clear_focus(m_seat);
        }
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
        if (!wlr_xdg_surface_try_from_wlr_surface(scene_surface->surface)) {
            return nullptr;
        }

        wlr_scene_tree* tree = node->parent;
        while (tree && !tree->node.data) {
            tree = tree->node.parent;
        }
        return tree ? static_cast<Window*>(tree->node.data) : nullptr;
    }

}
