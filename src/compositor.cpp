#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/popup.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"
#include "flux/workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <ranges>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace flux {

    Compositor::Compositor() = default;

    Compositor::~Compositor() {
        cancel_animations();
        if (m_xwayland) {
            m_xwayland_listeners.clear();
            wlr_xwayland_destroy(m_xwayland);
            m_xwayland = nullptr;
        }
        if (m_display) {
            wl_display_destroy_clients(m_display);
        }

        m_listeners.clear();

        m_popups.clear();
        m_windows.clear();
        m_outputs.clear();
        m_input.reset();
        m_config.reset();

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
        auto on_sigchld = [](int, void* data) {
            auto* self = static_cast<Compositor*>(data);
            siginfo_t info{};
            while (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid > 0) {
                if (self->xwayland() && self->xwayland()->server && info.si_pid == self->xwayland()->server->pid) {
                    break;
                }
                waitpid(info.si_pid, nullptr, 0);
                info = {};
            }
            return 0;
        };
        wl_event_loop_add_signal(event_loop, SIGCHLD, on_sigchld, this);

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

        auto* wlr_comp = wlr_compositor_create(m_display, 6, m_renderer);
        wlr_subcompositor_create(m_display);
        wlr_data_device_manager_create(m_display);
        wlr_primary_selection_v1_device_manager_create(m_display);
        m_xwayland = wlr_xwayland_create(m_display, wlr_comp, true);
        if (m_xwayland) {
            setenv("DISPLAY", m_xwayland->display_name, 1);
            wlr_log(WLR_INFO, "ready on X11 display: %s", m_xwayland->display_name);
            m_xwayland_listeners.listen(&m_xwayland->events.ready, [this](void*) {
                wlr_xwayland_set_seat(m_xwayland, m_input->seat().raw());
                m_input->cursor().set_xwayland_cursor(m_xwayland);
            });
            m_xwayland_listeners.listen(&m_xwayland->events.new_surface, [this](void* data) {
                auto* xsurface = static_cast<wlr_xwayland_surface*>(data);
                if (xsurface->override_redirect) {
                    create_popup(xsurface);
                } else {
                    create_window(xsurface);
                }
            });
        }
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
        wlr_alpha_modifier_v1_create(m_display);
        wlr_content_type_manager_v1_create(m_display, 1);
        wlr_color_representation_manager_v1_create_with_renderer(m_display, 1, m_renderer);
        wlr_fixes_create(m_display, 1);
        m_foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(m_display, 1);
        auto* foreign_registry = wlr_xdg_foreign_registry_create(m_display);
        wlr_xdg_foreign_v1_create(m_display, foreign_registry);
        wlr_xdg_foreign_v2_create(m_display, foreign_registry);
        m_tearing_control_mgr = wlr_tearing_control_manager_v1_create(m_display, 1);

        m_output_layout = wlr_output_layout_create(m_display);
        wlr_xdg_output_manager_v1_create(m_display, m_output_layout);

        m_workspace_manager = wlr_ext_workspace_manager_v1_create(m_display, 1);
        m_listeners.listen(&m_workspace_manager->events.commit, this, &Compositor::on_workspace_commit);

        auto* output_power_mgr = wlr_output_power_manager_v1_create(m_display);
        m_listeners.listen(&output_power_mgr->events.set_mode, this, &Compositor::on_output_power_set_mode);

        m_listeners.listen(&m_backend->events.new_output, this, &Compositor::on_new_output);

        m_scene = std::make_unique<Scene>(m_display, m_output_layout);

        if (const int drm_fd = wlr_renderer_get_drm_fd(m_renderer); drm_fd >= 0) {
            if (wlr_renderer_get_texture_formats(m_renderer, WLR_BUFFER_CAP_DMABUF) != nullptr) {
                if (auto* linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(m_display, 5, m_renderer)) {
                    m_scene->set_linux_dmabuf_v1(linux_dmabuf);
                }
            }
            if (m_renderer->features.timeline && m_backend->features.timeline) {
                wlr_linux_drm_syncobj_manager_v1_create(m_display, 1, drm_fd);
            }
        }

        auto* xdg_shell = wlr_xdg_shell_create(m_display, 7);
        m_listeners.listen(&xdg_shell->events.new_toplevel, this, &Compositor::on_new_window);
        m_listeners.listen(&xdg_shell->events.new_popup,
                           [this](void* d) { create_popup(static_cast<wlr_xdg_popup*>(d)); });

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
        m_config->set_on_reload([this] {
            m_input->apply_config();
            for (const auto& out : m_outputs) {
                for (const auto& ws : out->workspaces()) {
                    ws->arrange();
                }
            }
            m_input->cursor().update_focus();
        });

        const char* socket = wl_display_add_socket_auto(m_display);
        if (!socket) {
            return false;
        }

        if (!wlr_backend_start(m_backend)) {
            return false;
        }

        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);
        setenv("XDG_CURRENT_DESKTOP", "flux", 1);
        setenv("XDG_SESSION_DESKTOP", "flux", 1);
        wlr_log(WLR_INFO, "ready on Wayland socket: %s", socket);
        return true;
    }

    void Compositor::run() {
        if (m_session) {
            std::vector<std::string> args = {
                "--systemd", "WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "XDG_SESSION_DESKTOP",
            };
            if (m_xwayland) {
                args.emplace_back("DISPLAY");
            }
            for (const auto& [name, value] : m_config->environment()) {
                if (value && !std::ranges::contains(args, name)) {
                    args.push_back(name);
                }
            }
            spawn({
                .program = "dbus-update-activation-environment",
                .args = std::move(args),
            });
        }

        for (const auto& action : m_config->autostart()) {
            execute_action(action);
        }

        wl_display_run(m_display);
    }

    void Compositor::quit() noexcept {
        wl_display_terminate(m_display);
    }

    KeybindResult Compositor::handle_keybinding(uint32_t modifiers, xkb_keysym_t sym, bool is_repeat) {
        if (m_session && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            wlr_session_change_vt(m_session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            return {.handled = true, .repeat = false};
        }

        const bool is_locked = (exclusive_layer() != nullptr);
        const uint32_t clean_mods = modifiers & key_modifier_mask;
        const xkb_keysym_t lower_sym = xkb_keysym_to_lower(sym);

        for (const auto& bind : m_config->keybinds()) {
            if ((is_locked && !bind.locked) || (is_repeat && !bind.repeat)) {
                continue;
            }
            if (bind.modifiers == clean_mods && xkb_keysym_to_lower(bind.keysym) == lower_sym) {
                for (const auto& action : bind.actions) {
                    execute_action(action);
                }
                return {.handled = true, .repeat = bind.repeat};
            }
        }
        return {.handled = false, .repeat = false};
    }

    void Compositor::execute_action(const Action& action) {
        std::visit(
            [this](const auto& act) {
                using T = std::decay_t<decltype(act)>;
                if constexpr (!std::is_same_v<T, action::Quit> && !std::is_same_v<T, action::Spawn>) {
                    if (exclusive_layer()) {
                        return;
                    }
                }

                if constexpr (std::is_same_v<T, action::Quit>) {
                    quit();
                } else if constexpr (std::is_same_v<T, action::CloseWindow>) {
                    if (Window* win = focused_window()) {
                        win->close();
                    }
                } else if constexpr (std::is_same_v<T, action::CycleWindow>) {
                    if (Output* out = focused_output()) {
                        if (Window* next = out->active_workspace().cycle()) {
                            focus_window(next);
                        }
                    }
                } else if constexpr (std::is_same_v<T, action::MaximizeWindow>) {
                    if (Window* win = focused_window()) {
                        win->toggle_maximize();
                    }
                } else if constexpr (std::is_same_v<T, action::FullscreenWindow>) {
                    if (Window* win = focused_window()) {
                        win->toggle_fullscreen();
                    }
                } else if constexpr (std::is_same_v<T, action::ToggleFloating>) {
                    if (Window* win = focused_window()) {
                        win->toggle_floating();
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

            for (const auto& [name, value] : m_config->environment()) {
                if (value) {
                    setenv(name.c_str(), value->c_str(), 1);
                } else {
                    unsetenv(name.c_str());
                }
            }

            const int dev_null = open("/dev/null", O_RDWR);
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
            wlr_surface* new_root = surface ? root_popup_parent(surface) : nullptr;
            if (prev_root != new_root) {
                if (Window* prev_window = Window::try_from(prev_root)) {
                    prev_window->set_activated(false);
                } else if (wlr_xdg_toplevel* prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_root)) {
                    wlr_xdg_toplevel_set_activated(prev_toplevel, false);
                } else if (auto* prev_xsurface = wlr_xwayland_surface_try_from_wlr_surface(prev_root)) {
                    wlr_xwayland_surface_activate(prev_xsurface, false);
                }
            }
        }
        m_input->seat().notify_keyboard_enter(surface);
    }

    Window* Compositor::focused_window() const noexcept {
        Output* out = focused_output();
        return out ? out->active_workspace().focused_window() : nullptr;
    }

    void Compositor::focus_workspace(Output* output, uint32_t id) {
        if (exclusive_layer() || id == 0 || !output || output->active_workspace().id() == id) {
            return;
        }

        cancel_workspace_swipe();
        dismiss_popups();

        m_focused_output = output;
        wlr_log(WLR_DEBUG, "output '%s': switching from workspace %u to %u", output->raw()->name,
                output->active_workspace().id(), id);

        Workspace& old_ws = output->active_workspace();
        Workspace& new_ws = output->workspace(id);
        output->set_active_workspace(&new_ws);
        if (output->is_enabled()) {
            output->animation().start(old_ws, new_ws, std::max(1, output->layout_box().width));
            wlr_output_schedule_frame(output->raw());
        } else {
            old_ws.deactivate();
            new_ws.activate();
        }

        focus_window(new_ws.focused_window());
    }

    bool Compositor::begin_workspace_swipe() {
        if (m_swipe_output || !m_config->gestures().workspace_swipe || exclusive_layer()) {
            return false;
        }

        m_focused_output = nullptr;
        Output* out = focused_output();
        if (!out || !out->is_enabled()) {
            return false;
        }

        m_swipe_output = out;
        out->animation().begin_swipe(*out);
        dismiss_popups();
        restack_windows();
        return true;
    }

    void Compositor::update_workspace_swipe(double dx) {
        if (m_swipe_output) {
            m_swipe_output->animation().update_swipe(*m_swipe_output, dx, m_config->touchpad().natural_scroll);
        }
    }

    void Compositor::end_workspace_swipe() {
        if (!m_swipe_output) {
            return;
        }

        Output* out = m_swipe_output;
        m_swipe_output = nullptr;

        if (Workspace* target = out->animation().end_swipe(*out)) {
            m_focused_output = out;
            focus_window(target->focused_window());
        } else if (!out->animation().is_active()) {
            refocus();
        }
    }

    void Compositor::cancel_workspace_swipe() {
        if (m_swipe_output) {
            m_swipe_output->animation().cancel();
            m_swipe_output = nullptr;
        }
    }

    void Compositor::cancel_animations() {
        cancel_workspace_swipe();
        for (const auto& out : m_outputs) {
            out->animation().cancel();
        }
    }

    void Compositor::on_animation_finished() {
        restack_windows();
        m_input->cursor().update_focus();
    }

    void Compositor::move_to_workspace(Window* window, uint32_t id) {
        Workspace* old_ws = window ? window->workspace() : nullptr;
        if (exclusive_layer() || !old_ws || id == 0 || old_ws->id() == id) {
            return;
        }

        cancel_workspace_swipe();
        dismiss_popups(window->scene_tree());

        Output& out = old_ws->output();
        if (out.animation().is_active()) {
            out.animation().cancel();
        }
        wlr_log(WLR_DEBUG, "moving window to workspace %u on output '%s'", id, out.raw()->name);

        Workspace& new_ws = out.workspace(id);
        window->set_workspace(&new_ws);
        window->set_activated(false);

        old_ws->arrange();
        new_ws.arrange();
        refocus();
    }

    void Compositor::restack_windows() {
        for (const auto& out : m_outputs) {
            out->active_workspace().restack();
        }
    }

    void Compositor::focus_window(Window* window) {
        if (exclusive_layer()) {
            return;
        }

        if (window) {
            Workspace* ws = window->workspace();
            if (ws) {
                m_focused_output = &ws->output();
                ws->promote(window);

                if (ws != &ws->output().active_workspace()) {
                    focus_workspace(&ws->output(), ws->id());
                    return;
                }
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
        if (exclusive_layer()) {
            return;
        }

        Output* cur = focused_output();
        if (!cur) {
            return;
        }

        if (Window* w = cur->active_workspace().window_in_direction(direction)) {
            focus_window(w);
        } else if (Output* adj = cur->adjacent_output(direction)) {
            m_focused_output = adj;
            focus_window(adj->active_workspace().focused_window());
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
        if (m_focused_output) {
            return m_focused_output;
        }
        wlr_output* wlr_out =
            wlr_output_layout_output_at(m_output_layout, m_input->cursor().x(), m_input->cursor().y());
        if (!wlr_out) {
            wlr_out = wlr_output_layout_get_center_output(m_output_layout);
        }
        Output* out = output_from_wlr(wlr_out);
        return out ? out : (m_outputs.empty() ? nullptr : m_outputs.front().get());
    }

    void Compositor::map_window(Window* window) {
        if (Output* out = window->output() ? window->output() : focused_output()) {
            Workspace& ws = out->active_workspace();
            window->set_workspace(&ws);
            ws.arrange();
        }
        focus_window(window);
    }

    void Compositor::unmap_window(Window* window) {
        m_input->cursor().cancel_grab(window);
        dismiss_popups(window->scene_tree());
        Workspace* ws = window->workspace();
        if (ws) {
            ws->remove(window);
            ws->arrange();
        }
        refocus();
    }

    Layer* Compositor::exclusive_layer() const noexcept {
        constexpr std::array layers = {
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        };

        for (auto layer : layers) {
            for (const auto& out : m_outputs) {
                for (const auto& ls : std::views::reverse(out->layers())) {
                    if (ls->is_mapped() && ls->layer() == layer && ls->is_exclusive()) {
                        return ls.get();
                    }
                }
            }
        }

        return nullptr;
    }

    bool Compositor::allows_tearing(const Output* output) const noexcept {
        if (!output || exclusive_layer() || !m_popups.empty()) {
            return false;
        }

        const Window* w = output->active_workspace().focused_window();
        return w && w->is_fullscreen() &&
               wlr_tearing_control_manager_v1_surface_hint_from_surface(m_tearing_control_mgr, w->surface()) ==
                   WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
    }

    void Compositor::refocus() {
        if (auto* excl = exclusive_layer()) {
            restack_windows();
            focus_layer(excl);
            return;
        }

        if (wlr_surface* focused = m_input->seat().focused_surface()) {
            wlr_surface* root = root_popup_parent(focused);
            if (root && root->mapped) {
                const auto* layer = Layer::try_from(root);
                const auto* xsurf = wlr_xwayland_surface_try_from_wlr_surface(root);
                if ((layer && layer->is_focusable()) ||
                    (xsurf && xsurf->override_redirect && wlr_xwayland_surface_override_redirect_wants_focus(xsurf))) {
                    restack_windows();
                    m_input->cursor().update_focus();
                    return;
                }
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

    void Compositor::create_window(wlr_xwayland_surface* xsurface) {
        m_windows.push_back(std::make_unique<Window>(*this, xsurface));
    }

    void Compositor::create_popup(wlr_xwayland_surface* xsurface) {
        m_popups.push_back(std::make_unique<Popup>(*this, xsurface));
    }

    void Compositor::recreate_as_popup(Window* window) {
        if (!window) {
            return;
        }
        auto* xsurface = window->xwayland_surface();
        remove_window(window);
        if (xsurface) {
            create_popup(xsurface);
        }
    }

    void Compositor::recreate_as_window(Popup* popup) {
        if (!popup) {
            return;
        }
        auto* xsurface = popup->xwayland_surface();
        remove_popup(popup);
        if (xsurface) {
            create_window(xsurface);
        }
    }

    void Compositor::remove_popup(Popup* popup) {
        erase_unique_ptr(m_popups, popup);
    }

    void Compositor::dismiss_popups(wlr_scene_tree* root, wlr_scene_tree* except_root) {
        while (true) {
            auto it = std::ranges::find_if(m_popups, [root, except_root](const auto& p) {
                if (!p->raw()) {
                    return false;
                }
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
        if (!output) {
            return;
        }

        const bool is_nested = output->is_nested();
        if (m_swipe_output == output) {
            cancel_workspace_swipe();
        }
        output->animation().cancel();
        if (m_focused_output == output) {
            m_focused_output = nullptr;
        }

        auto it =
            std::ranges::find_if(m_outputs, [output](const auto& o) { return o.get() != output && o->is_enabled(); });
        if (it == m_outputs.end()) {
            it = std::ranges::find_if(m_outputs, [output](const auto& o) { return o.get() != output; });
        }
        Output* target = (it != m_outputs.end()) ? it->get() : nullptr;
        if (!target) {
            m_saved_workspace_id = output->active_workspace().id();
        }

        for (const auto& win : m_windows) {
            if (win->output() != output) {
                continue;
            }
            if (win->is_mapped()) {
                win->set_activated(false);
            }
            win->set_workspace(target ? &target->workspace(win->workspace_id()) : nullptr);
            win->set_placed(false);
        }

        erase_unique_ptr(m_outputs, output);

        if (is_nested && m_outputs.empty()) {
            quit();
            return;
        }

        for (const auto& remaining : m_outputs) {
            remaining->arrange_layers();
        }

        refocus();
    }

    void Compositor::on_new_output(wlr_output* output) {
        m_outputs.push_back(std::make_unique<Output>(*this, output));
        Output* new_out = m_outputs.back().get();
        if (m_outputs.size() == 1 && m_saved_workspace_id != 1) {
            new_out->active_workspace().deactivate();
            new_out->set_active_workspace(&new_out->workspace(m_saved_workspace_id));
            new_out->active_workspace().activate();
        }
        for (const auto& win : m_windows) {
            if (!win->workspace()) {
                win->set_workspace(&new_out->workspace(win->workspace_id()));
            }
        }
        for (const auto& out : m_outputs) {
            out->arrange_layers();
        }
        refocus();
    }

    void Compositor::on_new_window(wlr_xdg_toplevel* toplevel) {
        m_windows.push_back(std::make_unique<Window>(*this, toplevel));
    }

    void Compositor::on_request_activate(wlr_xdg_activation_v1_request_activate_event* event) {
        if (!event->surface->mapped) {
            return;
        }

        if (auto* window = Window::try_from(event->surface)) {
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
            focus_workspace(&to_activate->output(), to_activate->id());
        }
    }

    void Compositor::on_output_power_set_mode(wlr_output_power_v1_set_mode_event* event) {
        Output* output = output_from_wlr(event->output);
        if (!output) {
            return;
        }

        const bool enable = (event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
        if (output->is_enabled() == enable) {
            return;
        }

        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, enable);
        if (enable && !event->output->current_mode) {
            if (wlr_output_mode* mode = wlr_output_preferred_mode(event->output)) {
                wlr_output_state_set_mode(&state, mode);
            }
        }
        const bool committed = wlr_output_commit_state(event->output, &state);
        wlr_output_state_finish(&state);

        if (!committed) {
            return;
        }

        if (!enable && (m_swipe_output == output || output->animation().is_busy())) {
            if (m_swipe_output == output) {
                cancel_workspace_swipe();
            }
            output->animation().cancel();
            on_animation_finished();
        }
    }

    void Compositor::create_popup(wlr_xdg_popup* popup) {
        if (!popup || !popup->parent) {
            return;
        }

        wlr_scene_tree* root_tree = nullptr;
        wlr_surface* root_surface = root_popup_parent(popup->parent);
        if (wlr_xdg_surface* toplevel = wlr_xdg_surface_try_from_wlr_surface(root_surface);
            toplevel && toplevel->data) {
            root_tree = static_cast<wlr_scene_tree*>(toplevel->data);
        } else if (auto* ls = Layer::try_from(root_surface)) {
            root_tree = ls->scene_tree();
        }

        if (!root_tree) {
            wlr_log(WLR_ERROR, "popup parent surface has no associated scene tree");
            return;
        }

        m_popups.push_back(std::make_unique<Popup>(*this, popup, root_tree));
    }

    void Compositor::on_new_layer(wlr_layer_surface_v1* layer_surface) {
        Output* output = layer_surface->output ? output_from_wlr(layer_surface->output) : focused_output();
        if (!output) {
            wlr_log(WLR_ERROR, "no output available for layer surface");
            wlr_layer_surface_v1_destroy(layer_surface);
            return;
        }

        layer_surface->output = output->raw();
        output->add_layer(layer_surface);
    }

    void Compositor::focus_layer(Layer* layer) {
        if (!layer || !layer->is_mapped()) {
            return;
        }
        if (layer->output()) {
            m_focused_output = layer->output();
        }
        if (layer->is_exclusive()) {
            cancel_animations();
            dismiss_popups(nullptr, layer->scene_tree());
        }
        if (layer->scene_tree()) {
            wlr_scene_node_raise_to_top(&layer->scene_tree()->node);
        }
        focus_surface(layer->surface());
        m_input->cursor().update_focus();
    }

    void Compositor::update_cursor_focus() noexcept {
        m_input->cursor().update_focus();
    }

}
