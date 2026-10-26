#include "flux/server.hpp"
#include "flux/keyboard.hpp"
#include "flux/output.hpp"
#include "flux/popup.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ranges>
#include <string>
#include <unistd.h>

namespace flux {

    Server::Server() = default;

    Server::~Server() {
        if (m_display) {
            wl_display_destroy_clients(m_display);
        }

        m_listeners.clear();

        m_popups.clear();
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

    bool Server::init(std::optional<std::filesystem::path> config_path) {
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

        m_backend = wlr_backend_autocreate(wl_display_get_event_loop(m_display), nullptr);
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

        m_listeners.listen(&m_backend->events.new_output, this, &Server::on_new_output);

        m_scene = wlr_scene_create();
        m_scene_layout = wlr_scene_attach_output_layout(m_scene, m_output_layout);

        m_xdg_shell = wlr_xdg_shell_create(m_display, 7);
        m_listeners.listen(&m_xdg_shell->events.new_toplevel, this, &Server::on_new_window);
        m_listeners.listen(&m_xdg_shell->events.new_popup, this, &Server::on_new_popup);

        m_cursor = wlr_cursor_create();
        wlr_cursor_attach_output_layout(m_cursor, m_output_layout);

        m_cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

        m_cursor_state = cursor_state::Passthrough{};

        m_listeners.listen(&m_cursor->events.motion, this, &Server::on_cursor_motion);
        m_listeners.listen(&m_cursor->events.motion_absolute, this, &Server::on_cursor_motion_absolute);
        m_listeners.listen(&m_cursor->events.button, this, &Server::on_cursor_button);
        m_listeners.listen(&m_cursor->events.axis, this, &Server::on_cursor_axis);
        m_listeners.listen(&m_cursor->events.frame, this, &Server::on_cursor_frame);

        m_listeners.listen(&m_backend->events.new_input, this, &Server::on_new_input);

        m_seat = wlr_seat_create(m_display, "seat0");

        m_listeners.listen(&m_seat->events.request_set_cursor, this, &Server::on_request_cursor);
        m_listeners.listen(&m_seat->pointer_state.events.focus_change, this, &Server::on_pointer_focus_change);
        m_listeners.listen(&m_seat->events.request_set_selection, this, &Server::on_request_set_selection);

        const char* socket = wl_display_add_socket_auto(m_display);
        if (!socket) {
            return false;
        }

        if (!wlr_backend_start(m_backend)) {
            return false;
        }

        setenv("WAYLAND_DISPLAY", socket, 1);
        wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
        return true;
    }

    void Server::run() {
        wl_display_run(m_display);
    }

    bool Server::handle_keybinding(uint32_t modifiers, xkb_keysym_t sym) {
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

    void Server::execute_action(const Action& action) {
        std::visit(
            [this](const auto& act) {
                using T = std::decay_t<decltype(act)>;
                if constexpr (std::is_same_v<T, action::Quit>) {
                    wl_display_terminate(m_display);
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

    void Server::spawn(const action::Spawn& action) {
        if (fork() == 0) {
            setsid();

            std::vector<const char*> argv;
            argv.reserve(action.args.size() + 2);
            argv.push_back(action.program.c_str());
            for (const auto& arg : action.args) {
                argv.push_back(arg.c_str());
            }
            argv.push_back(nullptr);

            execvp(action.program.c_str(), const_cast<char* const*>(argv.data()));
            wlr_log(WLR_ERROR, "Failed to exec '%s': %s", action.program.c_str(), strerror(errno));
            _exit(1);
        }
    }

    void Server::focus_window(Window* window) {
        if (!window) {
            return;
        }
        wlr_surface* prev_surface = m_seat->keyboard_state.focused_surface;
        wlr_surface* surface = window->surface();
        if (prev_surface == surface) {
            return;
        }
        if (prev_surface) {
            wlr_xdg_toplevel* prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
            if (prev_toplevel != nullptr) {
                wlr_xdg_toplevel_set_activated(prev_toplevel, false);
            }
        }

        wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_seat);
        window->raise_to_top();

        std::erase(m_focus_stack, window);
        m_focus_stack.insert(m_focus_stack.begin(), window);

        window->set_activated(true);
        if (keyboard != nullptr) {
            wlr_seat_keyboard_notify_enter(m_seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                           &keyboard->modifiers);
        }
    }

    void Server::begin_move(Window* window) {
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

    void Server::begin_resize(Window* window, uint32_t edges) {
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

    void Server::reset_cursor_mode() {
        if (Window* window = grabbed_window()) {
            window->apply_pending_position();
        }
        m_cursor_state = cursor_state::Passthrough{};
        wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "default");
        process_cursor_motion(0);
    }

    Window* Server::grabbed_window() const noexcept {
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

    void Server::center_window(Window* window) {
        if (!window || !m_output_layout || !m_cursor) {
            return;
        }

        wlr_output* output = wlr_output_layout_output_at(m_output_layout, m_cursor->x, m_cursor->y);
        if (!output) {
            output = wlr_output_layout_get_center_output(m_output_layout);
        }
        if (!output) {
            return;
        }

        wlr_box output_box{};
        wlr_output_layout_get_box(m_output_layout, output, &output_box);

        const wlr_box& geo = window->geometry();
        const int width = geo.width > 0 ? geo.width : window->surface()->current.width;
        const int height = geo.height > 0 ? geo.height : window->surface()->current.height;

        const int center_x = output_box.x + std::max(0, (output_box.width - width) / 2);
        const int center_y = output_box.y + std::max(0, (output_box.height - height) / 2);

        window->set_position(center_x - geo.x, center_y - geo.y);
    }

    void Server::map_window(Window* window) {
        center_window(window);
        if (std::ranges::find(m_focus_stack, window) == m_focus_stack.end()) {
            m_focus_stack.insert(m_focus_stack.begin(), window);
        }
        focus_window(window);
    }

    void Server::unmap_window(Window* window) {
        if (window == grabbed_window()) {
            reset_cursor_mode();
        }
        std::erase(m_focus_stack, window);
        if (!m_focus_stack.empty()) {
            focus_window(m_focus_stack.front());
        } else {
            wlr_seat_keyboard_notify_clear_focus(m_seat);
        }
    }

    void Server::remove_window(Window* window) {
        unmap_window(window);
        erase_unique_ptr(m_windows, window);
    }

    void Server::remove_popup(Popup* popup) {
        erase_unique_ptr(m_popups, popup);
    }

    void Server::remove_output(Output* output) {
        erase_unique_ptr(m_outputs, output);
    }

    void Server::remove_keyboard(Keyboard* keyboard) {
        if (wlr_seat_get_keyboard(m_seat) == keyboard->raw()) {
            wlr_keyboard* next_keyboard = nullptr;
            for (const auto& kbd : m_keyboards) {
                if (kbd.get() != keyboard) {
                    next_keyboard = kbd->raw();
                    break;
                }
            }
            wlr_seat_set_keyboard(m_seat, next_keyboard);
        }

        erase_unique_ptr(m_keyboards, keyboard);
        uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
        if (!m_keyboards.empty()) {
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        }
        wlr_seat_set_capabilities(m_seat, caps);
    }

    void Server::on_new_output(wlr_output* output) {
        m_outputs.push_back(std::make_unique<Output>(*this, output));
    }

    void Server::on_new_window(wlr_xdg_toplevel* toplevel) {
        m_windows.push_back(std::make_unique<Window>(*this, toplevel));
    }

    void Server::on_new_popup(wlr_xdg_popup* popup) {
        m_popups.push_back(std::make_unique<Popup>(*this, popup));
    }

    void Server::on_cursor_motion(wlr_pointer_motion_event* event) {
        wlr_cursor_move(m_cursor, &event->pointer->base, event->delta_x, event->delta_y);
        process_cursor_motion(event->time_msec);
    }

    void Server::on_cursor_motion_absolute(wlr_pointer_motion_absolute_event* event) {
        wlr_cursor_warp_absolute(m_cursor, &event->pointer->base, event->x, event->y);
        process_cursor_motion(event->time_msec);
    }

    void Server::on_cursor_button(wlr_pointer_button_event* event) {
        if (std::holds_alternative<cursor_state::Passthrough>(m_cursor_state)) {
            if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
                double sx = 0;
                double sy = 0;
                wlr_surface* surface = nullptr;
                Window* window = window_at(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
                focus_window(window);
            }
            wlr_seat_pointer_notify_button(m_seat, event->time_msec, event->button, event->state);
        } else {
            wlr_seat_pointer_notify_button(m_seat, event->time_msec, event->button, event->state);
            if (m_seat->pointer_state.button_count == 0) {
                reset_cursor_mode();
            }
        }
    }

    void Server::on_cursor_axis(wlr_pointer_axis_event* event) {
        wlr_seat_pointer_notify_axis(m_seat, event->time_msec, event->orientation, event->delta, event->delta_discrete,
                                     event->source, event->relative_direction);
    }

    void Server::on_cursor_frame() {
        wlr_seat_pointer_notify_frame(m_seat);
    }

    void Server::on_new_input(wlr_input_device* device) {
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

    void Server::on_request_cursor(wlr_seat_pointer_request_set_cursor_event* event) {
        wlr_seat_client* focused_client = m_seat->pointer_state.focused_client;
        if (focused_client == event->seat_client && std::holds_alternative<cursor_state::Passthrough>(m_cursor_state)) {
            wlr_cursor_set_surface(m_cursor, event->surface, event->hotspot_x, event->hotspot_y);
        }
    }

    void Server::on_pointer_focus_change(wlr_seat_pointer_focus_change_event* event) {
        if (event->new_surface == nullptr && std::holds_alternative<cursor_state::Passthrough>(m_cursor_state)) {
            wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "default");
        }
    }

    void Server::on_request_set_selection(wlr_seat_request_set_selection_event* event) {
        wlr_seat_set_selection(m_seat, event->source, event->serial);
    }

    void Server::process_cursor_move(const cursor_state::Move& state) {
        state.window->set_position(m_cursor->x - state.grab_x, m_cursor->y - state.grab_y);
    }

    void Server::process_cursor_resize(const cursor_state::Resize& state) {
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

        int min_w = std::max(1, state.window->raw()->current.min_width);
        int min_h = std::max(1, state.window->raw()->current.min_height);
        int max_w = state.window->raw()->current.max_width;
        int max_h = state.window->raw()->current.max_height;

        int new_w = new_right - new_left;
        int new_h = new_bottom - new_top;

        if (new_w < min_w) {
            if (state.resize_edges & WLR_EDGE_LEFT) {
                new_left = new_right - min_w;
            } else {
                new_right = new_left + min_w;
            }
            new_w = min_w;
        }
        if (max_w > 0 && new_w > max_w) {
            if (state.resize_edges & WLR_EDGE_LEFT) {
                new_left = new_right - max_w;
            } else {
                new_right = new_left + max_w;
            }
            new_w = max_w;
        }

        if (new_h < min_h) {
            if (state.resize_edges & WLR_EDGE_TOP) {
                new_top = new_bottom - min_h;
            } else {
                new_bottom = new_top + min_h;
            }
            new_h = min_h;
        }
        if (max_h > 0 && new_h > max_h) {
            if (state.resize_edges & WLR_EDGE_TOP) {
                new_top = new_bottom - max_h;
            } else {
                new_bottom = new_top + max_h;
            }
            new_h = max_h;
        }

        const wlr_box& geo_box = state.window->geometry();
        if (state.resize_edges & (WLR_EDGE_TOP | WLR_EDGE_LEFT)) {
            state.window->set_pending_position(new_left - geo_box.x, new_top - geo_box.y);
        }
        state.window->set_size(new_w, new_h);
    }

    void Server::process_cursor_motion(uint32_t time_msec) {
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
        if (!window) {
            wlr_cursor_set_xcursor(m_cursor, m_cursor_mgr, "default");
        }
        if (surface) {
            wlr_seat_pointer_notify_enter(m_seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(m_seat, time_msec, sx, sy);
        } else {
            wlr_seat_pointer_clear_focus(m_seat);
        }
    }

    Window* Server::window_at(double lx, double ly, wlr_surface** surface, double* sx, double* sy) {
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
        while (tree != nullptr && tree->node.data == nullptr) {
            tree = tree->node.parent;
        }
        if (!tree) {
            return nullptr;
        }
        return static_cast<Window*>(tree->node.data);
    }

}
