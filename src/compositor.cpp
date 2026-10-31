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
        m_windows.clear();
        m_workspaces.clear();
        if (m_workspace_group) {
            wlr_ext_workspace_group_handle_v1_destroy(m_workspace_group);
        }
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

        auto* workspace_mgr = wlr_ext_workspace_manager_v1_create(m_display, 1);
        m_workspace_group = wlr_ext_workspace_group_handle_v1_create(workspace_mgr, 0);
        m_listeners.listen(&workspace_mgr->events.commit, this, &Compositor::on_workspace_commit);

        m_listeners.listen(&m_backend->events.new_output, this, &Compositor::on_new_output);

        m_scene = std::make_unique<Scene>(m_display, m_output_layout);
        m_current_workspace = &workspace(1);
        m_current_workspace->activate();
        for (uint32_t i = 2; i <= 9; ++i) {
            workspace(i);
        }

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

        auto* xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(m_display);
        m_listeners.listen(&xdg_decoration_mgr->events.new_toplevel_decoration, [](void* data) {
            auto* decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);
            if (auto* window = Window::try_from(decoration->toplevel)) {
                window->set_decoration(decoration);
            }
        });

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
                    if (Window* win = focused_window()) {
                        win->close();
                    }
                } else if constexpr (std::is_same_v<T, action::CycleWindow>) {
                    if (Window* next = m_current_workspace->cycle()) {
                        focus_window(next);
                    }
                } else if constexpr (std::is_same_v<T, action::MaximizeWindow>) {
                    if (Window* win = focused_window()) {
                        win->toggle_maximize();
                    }
                } else if constexpr (std::is_same_v<T, action::FullscreenWindow>) {
                    if (Window* win = focused_window()) {
                        win->toggle_fullscreen();
                    }
                } else if constexpr (std::is_same_v<T, action::FocusWindow>) {
                    focus_window_in_direction(act.direction);
                } else if constexpr (std::is_same_v<T, action::FocusWorkspace>) {
                    focus_workspace(act.workspace);
                } else if constexpr (std::is_same_v<T, action::MoveToWorkspace>) {
                    if (Window* win = focused_window()) {
                        move_to_workspace(win, act.workspace);
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

    Workspace& Compositor::workspace(uint32_t id) {
        auto it = std::ranges::find_if(m_workspaces, [id](const auto& ws) { return ws->id() == id; });
        if (it != m_workspaces.end()) {
            return **it;
        }

        m_workspaces.push_back(std::make_unique<Workspace>(id, *m_scene, m_workspace_group));
        return *m_workspaces.back();
    }

    void Compositor::focus_workspace(uint32_t id) {
        if (exclusive_layer() || id == 0 || m_current_workspace->id() == id) {
            return;
        }

        dismiss_popups();

        wlr_log(WLR_DEBUG, "switching from workspace %u to %u", m_current_workspace->id(), id);
        m_current_workspace->deactivate();

        m_current_workspace = &workspace(id);
        m_current_workspace->activate();

        focus_window(focused_window());
    }

    void Compositor::move_to_workspace(Window* window, uint32_t id) {
        if (exclusive_layer() || !window || id == 0 || window->workspace()->id() == id) {
            return;
        }

        dismiss_popups(window->scene_tree());

        wlr_log(WLR_DEBUG, "moving window to workspace %u", id);
        Workspace* old_ws = window->workspace();
        old_ws->remove(window);

        Workspace& new_ws = workspace(id);
        new_ws.promote(window);
        window->set_workspace(&new_ws);
        window->set_activated(false);

        arrange_workspace(old_ws);
        arrange_workspace(&new_ws);

        refocus();
    }

    void Compositor::restack_windows() {
        std::vector<Output*> seen_outputs;
        for (Window* w : m_current_workspace->focus_stack()) {
            Output* out = w->output();
            const bool is_top_on_output = out && !std::ranges::contains(seen_outputs, out);
            if (is_top_on_output) {
                seen_outputs.push_back(out);
                w->raise_to_top();
            }
            if (w->is_fullscreen()) {
                if (is_top_on_output) {
                    wlr_scene_node_reparent(&w->scene_tree()->node, m_scene->fullscreen());
                } else {
                    w->reparent_to_workspace();
                }
            }
            if (!is_top_on_output && !w->is_tiled()) {
                w->lower_to_bottom();
            }
        }
    }

    void Compositor::focus_window(Window* window) {
        if (exclusive_layer()) {
            return;
        }

        if (window) {
            Workspace* ws = window->workspace();
            ws->promote(window);

            if (ws != m_current_workspace) {
                focus_workspace(ws->id());
                return;
            }
        }

        restack_windows();

        if (!window) {
            focus_surface(nullptr);
            m_input->cursor().update_focus();
            return;
        }

        window->set_activated(true);
        focus_surface(window->surface());
        m_input->cursor().update_focus();
    }

    void Compositor::focus_window_in_direction(Direction direction) {
        Window* focused = focused_window();
        if (!focused || !focused->output()) {
            return;
        }

        Output* cur_output = focused->output();

        auto focus_adjacent_output = [this, cur_output, direction] -> bool {
            const wlr_box cur_box = cur_output->layout_box();
            const double ref_lx = cur_box.x + (static_cast<double>(cur_box.width) / 2.0);
            const double ref_ly = cur_box.y + (static_cast<double>(cur_box.height) / 2.0);

            wlr_output* adj_wlr = wlr_output_layout_adjacent_output(
                m_output_layout, static_cast<wlr_direction>(direction), cur_output->raw(), ref_lx, ref_ly);
            Output* adj_out = output_from_wlr(adj_wlr);
            if (!adj_out || !adj_out->is_enabled()) {
                return false;
            }

            for (Window* w : m_current_workspace->focus_stack()) {
                if (w->output() == adj_out) {
                    focus_window(w);
                    return true;
                }
            }
            return false;
        };

        if (!focused->is_tiled()) {
            focus_adjacent_output();
            return;
        }

        std::vector<Window*> tiled;
        for (Window* w : m_current_workspace->windows()) {
            if (w->output() == cur_output && w->is_tiled()) {
                tiled.push_back(w);
            }
        }

        if (tiled.size() <= 1) {
            focus_adjacent_output();
            return;
        }

        const auto it = std::ranges::find(tiled, focused);
        const size_t idx = static_cast<size_t>(std::distance(tiled.begin(), it));

        if (idx == 0) {
            if (direction == Direction::Right) {
                Window* target = tiled[1];
                for (Window* w : m_current_workspace->focus_stack()) {
                    if (w != focused && w->output() == cur_output && w->is_tiled()) {
                        target = w;
                        break;
                    }
                }
                focus_window(target);
            } else if (direction == Direction::Left) {
                focus_adjacent_output();
            } else if (direction == Direction::Down) {
                if (!focus_adjacent_output()) {
                    focus_window(tiled[1]);
                }
            } else if (direction == Direction::Up) {
                if (!focus_adjacent_output()) {
                    focus_window(tiled.back());
                }
            }
        } else {
            if (direction == Direction::Left) {
                focus_window(tiled[0]);
            } else if (direction == Direction::Right) {
                focus_adjacent_output();
            } else if (direction == Direction::Up) {
                if (idx == 1 && focus_adjacent_output()) {
                    return;
                }
                focus_window(tiled[(idx == 1) ? (tiled.size() - 1) : (idx - 1)]);
            } else if (direction == Direction::Down) {
                if (idx == tiled.size() - 1 && focus_adjacent_output()) {
                    return;
                }
                focus_window(tiled[(idx == tiled.size() - 1) ? 1 : (idx + 1)]);
            }
        }
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

    void Compositor::map_window(Window* window) {
        if (!window->output()) {
            window->set_output(focused_output());
        }
        window->set_workspace(m_current_workspace);
        m_current_workspace->promote(window);
        arrange_workspace(m_current_workspace);
        focus_window(window);
    }

    void Compositor::unmap_window(Window* window) {
        dismiss_popups(window->scene_tree());
        Workspace* ws = window->workspace();
        ws->remove(window);
        arrange_workspace(ws);
        refocus();
    }

    void Compositor::arrange_workspace(Workspace* ws) {
        if (!ws) {
            return;
        }

        for (const auto& output : m_outputs) {
            if (!output->is_enabled()) {
                continue;
            }

            const wlr_box full = output->layout_box();
            const wlr_box& usable = output->usable_area();

            std::vector<Window*> tiled;
            for (Window* w : ws->windows()) {
                if (w->output() != output.get()) {
                    continue;
                }
                if (w->is_fullscreen()) {
                    w->set_geometry(full);
                } else if (w->is_maximized()) {
                    w->set_geometry(usable);
                } else {
                    tiled.push_back(w);
                }
            }

            for (size_t i = 0; i < tiled.size(); ++i) {
                tiled[i]->set_geometry(tile_box(usable, i, tiled.size()));
            }
        }
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
            restack_windows();
            focus_layer(excl);
            return;
        }

        if (wlr_surface* focused = m_input->seat().focused_surface()) {
            if (auto* layer = Layer::try_from(root_popup_parent(focused));
                layer && layer->is_mapped() && layer->is_focusable()) {
                restack_windows();
                m_input->cursor().update_focus();
                return;
            }
        }

        focus_window(focused_window());
    }

    void Compositor::remove_window(Window* window) {
        if (window) {
            if (window->is_mapped()) {
                unmap_window(window);
            } else {
                dismiss_popups(window->scene_tree());
            }
        }
        erase_unique_ptr(m_windows, window);
    }

    void Compositor::remove_popup(Popup* popup) {
        erase_unique_ptr(m_popups, popup);
    }

    void Compositor::dismiss_popups(wlr_scene_tree* root, wlr_scene_tree* except_root) {
        while (true) {
            auto it = std::ranges::find_if(m_popups, [root, except_root](const auto& p) {
                if (except_root && p->root_tree() == except_root) {
                    return false;
                }
                return !root || p->root_tree() == root;
            });
            if (it == m_popups.end()) {
                break;
            }
            wlr_xdg_popup_destroy((*it)->raw());
        }
    }

    void Compositor::remove_output(Output* output) {
        const bool is_nested = output && output->is_nested();

        if (output) {
            wlr_ext_workspace_group_handle_v1_output_leave(m_workspace_group, output->raw());
        }

        std::vector<wlr_layer_surface_v1*> to_destroy;
        for (const auto& ls : m_layers) {
            if (ls->output() == output) {
                to_destroy.push_back(ls->raw());
            }
        }
        erase_unique_ptr(m_outputs, output);

        Output* target = focused_output();
        for (const auto& win : m_windows) {
            if (win->output() == output) {
                win->set_output(target);
            }
        }

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

        refocus();
    }

    void Compositor::remove_layer(Layer* layer) {
        if (!layer) {
            return;
        }

        const bool was_initialized = layer->is_initialized();
        dismiss_popups(layer->scene_tree());
        Output* output = layer->output();
        erase_unique_ptr(m_layers, layer);

        if (was_initialized) {
            if (output) {
                arrange_layers(output);
            }
            refocus();
        }
    }

    void Compositor::on_new_output(wlr_output* output) {
        m_outputs.push_back(std::make_unique<Output>(*this, output));
        Output* new_out = m_outputs.back().get();
        wlr_ext_workspace_group_handle_v1_output_enter(m_workspace_group, output);
        for (const auto& win : m_windows) {
            if (!win->output()) {
                win->set_output(new_out);
            }
        }
        for (const auto& out : m_outputs) {
            arrange_layers(out.get());
        }
    }

    void Compositor::on_new_window(wlr_xdg_toplevel* toplevel) {
        m_windows.push_back(std::make_unique<Window>(*this, toplevel));
    }

    void Compositor::on_request_activate(wlr_xdg_activation_v1_request_activate_event* event) {
        if (!event->surface->mapped) {
            return;
        }

        if (auto* window = Window::try_from(wlr_xdg_toplevel_try_from_wlr_surface(event->surface))) {
            focus_window(window);
        }
    }

    void Compositor::on_workspace_commit(wlr_ext_workspace_v1_commit_event* event) {
        Workspace* to_activate = nullptr;
        wlr_ext_workspace_v1_request* req = nullptr;
        wl_list_for_each(req, event->requests, link) {
            if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE && req->activate.workspace) {
                to_activate = static_cast<Workspace*>(req->activate.workspace->data);
            }
        }

        if (to_activate) {
            focus_workspace(to_activate->id());
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

        for (const auto& popup : m_popups) {
            popup->update_position();
        }

        for (const auto& ws : m_workspaces) {
            arrange_workspace(ws.get());
        }

        m_input->cursor().update_focus();
    }

    void Compositor::focus_layer(Layer* layer) {
        if (!layer || !layer->is_mapped()) {
            return;
        }
        if (layer->is_exclusive()) {
            dismiss_popups(nullptr, layer->scene_tree());
        }
        if (layer->scene_tree()) {
            wlr_scene_node_raise_to_top(&layer->scene_tree()->node);
        }
        focus_surface(layer->surface());
        m_input->cursor().update_focus();
    }

}
