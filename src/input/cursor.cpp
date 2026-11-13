#include "flux/input/cursor.hpp"
#include "flux/compositor.hpp"
#include "flux/input.hpp"
#include "flux/layer.hpp"
#include "flux/output.hpp"
#include "flux/util.hpp"
#include "flux/window.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

namespace flux::input {

    Cursor::Cursor(Input& input)
        : m_input(input), m_wlr_cursor(wlr_cursor_create()),
          m_pointer_gestures(wlr_pointer_gestures_v1_create(m_input.compositor().display())),
          m_relative_pointer_mgr(wlr_relative_pointer_manager_v1_create(m_input.compositor().display())) {
        if (const char* env_theme = std::getenv("XCURSOR_THEME"); env_theme && *env_theme != '\0') {
            m_env_xcursor_theme = env_theme;
        }
        wlr_cursor_attach_output_layout(m_wlr_cursor, m_input.compositor().output_layout());
        apply_config(m_input.compositor().config().cursor());

        m_listeners.listen(&m_wlr_cursor->events.motion, this, &Cursor::on_motion);
        m_listeners.listen(&m_wlr_cursor->events.motion_absolute, this, &Cursor::on_motion_absolute);
        m_listeners.listen(&m_wlr_cursor->events.button, this, &Cursor::on_button);
        m_listeners.listen(&m_wlr_cursor->events.axis, this, &Cursor::on_axis);
        m_listeners.listen(&m_wlr_cursor->events.frame, this, &Cursor::on_frame);
        m_listeners.listen(&m_wlr_cursor->events.swipe_begin, this, &Cursor::on_swipe_begin);
        m_listeners.listen(&m_wlr_cursor->events.swipe_update, this, &Cursor::on_swipe_update);
        m_listeners.listen(&m_wlr_cursor->events.swipe_end, this, &Cursor::on_swipe_end);
        m_listeners.listen(&m_wlr_cursor->events.pinch_begin, [this](void* d) {
            auto* ev = static_cast<wlr_pointer_pinch_begin_event*>(d);
            m_input.seat().notify_activity();
            wlr_pointer_gestures_v1_send_pinch_begin(m_pointer_gestures, m_input.seat().raw(), ev->time_msec,
                                                     ev->fingers);
        });
        m_listeners.listen(&m_wlr_cursor->events.pinch_update, [this](void* d) {
            auto* ev = static_cast<wlr_pointer_pinch_update_event*>(d);
            m_input.seat().notify_activity();
            wlr_pointer_gestures_v1_send_pinch_update(m_pointer_gestures, m_input.seat().raw(), ev->time_msec, ev->dx,
                                                      ev->dy, ev->scale, ev->rotation);
        });
        m_listeners.listen(&m_wlr_cursor->events.pinch_end, [this](void* d) {
            auto* ev = static_cast<wlr_pointer_pinch_end_event*>(d);
            m_input.seat().notify_activity();
            wlr_pointer_gestures_v1_send_pinch_end(m_pointer_gestures, m_input.seat().raw(), ev->time_msec,
                                                   ev->cancelled);
        });
        m_listeners.listen(&m_wlr_cursor->events.hold_begin, [this](void* d) {
            auto* ev = static_cast<wlr_pointer_hold_begin_event*>(d);
            m_input.seat().notify_activity();
            wlr_pointer_gestures_v1_send_hold_begin(m_pointer_gestures, m_input.seat().raw(), ev->time_msec,
                                                    ev->fingers);
        });
        m_listeners.listen(&m_wlr_cursor->events.hold_end, [this](void* d) {
            auto* ev = static_cast<wlr_pointer_hold_end_event*>(d);
            m_input.seat().notify_activity();
            wlr_pointer_gestures_v1_send_hold_end(m_pointer_gestures, m_input.seat().raw(), ev->time_msec,
                                                  ev->cancelled);
        });
    }

    Cursor::~Cursor() {
        m_listeners.clear();
        if (m_cursor_mgr) {
            wlr_xcursor_manager_destroy(m_cursor_mgr);
        }
        if (m_wlr_cursor) {
            wlr_cursor_destroy(m_wlr_cursor);
        }
    }

    void Cursor::apply_config(const CursorConfig& config) {
        if (m_cursor_mgr && m_config == config) {
            return;
        }
        m_config = config;

        const char* theme = !config.theme.empty() ? config.theme.c_str()
                                                  : (m_env_xcursor_theme ? m_env_xcursor_theme->c_str() : nullptr);
        if (theme) {
            setenv("XCURSOR_THEME", theme, 1);
        } else {
            unsetenv("XCURSOR_THEME");
        }
        setenv("XCURSOR_SIZE", std::to_string(config.size).c_str(), 1);
        if (m_cursor_mgr) {
            wlr_xcursor_manager_destroy(m_cursor_mgr);
        }
        m_cursor_mgr = wlr_xcursor_manager_create(theme, config.size);
        wlr_xcursor_manager_load(m_cursor_mgr, 1);

        if (m_current_shape) {
            wlr_cursor_set_xcursor(m_wlr_cursor, m_cursor_mgr, m_current_shape);
        } else if (!m_input.seat().focused_pointer_surface()) {
            set_xcursor("default");
        }
        if (auto* xwayland = m_input.compositor().xwayland()) {
            set_xwayland_cursor(xwayland);
        }
    }

    void Cursor::set_xwayland_cursor(wlr_xwayland* xwayland) {
        if (!xwayland || !m_cursor_mgr) {
            return;
        }
        wlr_xcursor* xcursor = wlr_xcursor_manager_get_xcursor(m_cursor_mgr, "default", 1);
        if (xcursor && xcursor->image_count > 0) {
            wlr_xcursor_image* image = xcursor->images[0];
            wlr_buffer* buffer = wlr_xcursor_image_get_buffer(image);
            wlr_xwayland_set_cursor(xwayland, buffer, static_cast<int32_t>(image->hotspot_x),
                                    static_cast<int32_t>(image->hotspot_y));
        }
    }

    void Cursor::start_grab(Window* window, uint32_t button) {
        uint32_t edges = WLR_EDGE_NONE;
        if (button == btn_right) {
            const wlr_box box = window->geometry_box();
            const uint32_t horiz = (m_wlr_cursor->x < box.x + (box.width / 2.0)) ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
            const uint32_t vert = (m_wlr_cursor->y < box.y + (box.height / 2.0)) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
            edges = horiz | vert;
        }

        m_grab_window = window;
        m_grab_button = button;
        m_grab_x = m_wlr_cursor->x;
        m_grab_y = m_wlr_cursor->y;
        m_input.seat().clear_pointer_focus();
        set_xcursor(edges != WLR_EDGE_NONE ? wlr_xcursor_get_resize_name(static_cast<wlr_edges>(edges)) : "grabbing");
        window->start_grab(edges);
    }

    void Cursor::end_grab() {
        if (!m_grab_window) {
            return;
        }
        Window* window = m_grab_window;
        m_grab_window = nullptr;
        m_grab_button = 0;
        window->end_grab();
        set_xcursor("default");
        update_focus();
    }

    void Cursor::cancel_grab(const Window* window) {
        if (m_grab_window && (!window || m_grab_window == window)) {
            end_grab();
        }
    }

    bool Cursor::apply_constraint(double& dx, double& dy) const {
        auto* constraint = m_input.seat().active_constraint();
        if (!constraint) {
            return true;
        }
        if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            return false;
        }
        double sx = 0.0;
        double sy = 0.0;
        if (m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy) !=
            constraint->surface) {
            return true;
        }

        const pixman_region32_t* region =
            pixman_region32_not_empty(&constraint->region) || pixman_region32_not_empty(&constraint->current.region)
                ? &constraint->region
                : &constraint->surface->input_region;
        double confined_x = 0.0;
        double confined_y = 0.0;
        if (!wlr_region_confine(region, sx, sy, sx + dx, sy + dy, &confined_x, &confined_y)) {
            return false;
        }
        dx = confined_x - sx;
        dy = confined_y - sy;
        return true;
    }

    void Cursor::handle_motion(wlr_input_device* device, uint32_t time_msec, double dx, double dy, double unaccel_dx,
                               double unaccel_dy) {
        m_input.seat().notify_activity();
        wlr_relative_pointer_manager_v1_send_relative_motion(m_relative_pointer_mgr, m_input.seat().raw(),
                                                             static_cast<uint64_t>(time_msec) * 1000, dx, dy,
                                                             unaccel_dx, unaccel_dy);
        if (apply_constraint(dx, dy)) {
            wlr_cursor_move(m_wlr_cursor, device, dx, dy);
            if (!m_grab_window && !m_input.compositor().focused_window()) {
                m_input.compositor().set_focused_output(nullptr);
            }
            process_motion(time_msec);
        }
    }

    void Cursor::on_motion(wlr_pointer_motion_event* event) {
        handle_motion(&event->pointer->base, event->time_msec, event->delta_x, event->delta_y, event->unaccel_dx,
                      event->unaccel_dy);
    }

    void Cursor::on_motion_absolute(wlr_pointer_motion_absolute_event* event) {
        double lx = 0.0;
        double ly = 0.0;
        wlr_cursor_absolute_to_layout_coords(m_wlr_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
        const double dx = lx - m_wlr_cursor->x;
        const double dy = ly - m_wlr_cursor->y;
        handle_motion(&event->pointer->base, event->time_msec, dx, dy, dx, dy);
    }

    void Cursor::on_button(wlr_pointer_button_event* event) {
        if (m_grab_window) {
            m_input.seat().notify_activity();
            if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && event->button == m_grab_button) {
                m_input.compositor().set_focused_output(nullptr);
                end_grab();
            }
            return;
        }

        if (m_input.compositor().is_workspace_swiping()) {
            m_input.seat().notify_activity();
            return;
        }

        if (m_input.seat().is_drag_active()) {
            m_input.seat().notify_button(event->time_msec, event->button, event->state);
            return;
        }

        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            m_input.compositor().set_focused_output(nullptr);
            if (Output* out = m_input.compositor().focused_output(); out && out->animation().is_active()) {
                out->animation().cancel();
                m_input.compositor().on_animation_finished();
            }

            double sx = 0.0;
            double sy = 0.0;
            wlr_surface* surface = nullptr;
            Window* window =
                m_input.compositor().scene().window_at(m_wlr_cursor->x, m_wlr_cursor->y, &surface, &sx, &sy);
            if (const auto* excl = m_input.compositor().exclusive_layer()) {
                const auto* layer = Layer::try_from(root_popup_parent(surface));
                if (layer && layer->is_mapped() && layer->is_exclusive() && layer->layer() == excl->layer()) {
                    m_input.compositor().focus_surface(surface);
                }
            } else if (window != nullptr) {
                m_input.compositor().focus_window(window);

                wlr_keyboard* keyboard = m_input.seat().keyboard();
                const uint32_t mods = keyboard ? (wlr_keyboard_get_modifiers(keyboard) & key_modifier_mask) : 0;
                if (mods == WLR_MODIFIER_LOGO && (event->button == btn_left || event->button == btn_right) &&
                    window->is_floating()) {
                    m_input.seat().notify_activity();
                    start_grab(window, event->button);
                    return;
                }
            } else if (surface != nullptr) {
                if (auto* layer = Layer::try_from(root_popup_parent(surface)); layer && layer->is_focusable()) {
                    m_input.compositor().focus_surface(surface);
                } else if (auto* xsurf = wlr_xwayland_surface_try_from_wlr_surface(surface);
                           xsurf && xsurf->override_redirect &&
                           wlr_xwayland_surface_override_redirect_wants_focus(xsurf)) {
                    wlr_xwayland_surface_activate(xsurf, true);
                    m_input.compositor().focus_surface(surface);
                } else {
                    m_input.compositor().focus_window(m_input.compositor().focused_window());
                }
            } else {
                m_input.compositor().focus_window(nullptr);
            }
        }
        m_input.seat().notify_button(event->time_msec, event->button, event->state);
    }

    void Cursor::on_axis(wlr_pointer_axis_event* event) {
        m_input.seat().notify_axis(event->time_msec, event->orientation, event->delta, event->delta_discrete,
                                   event->source, event->relative_direction);
    }

    void Cursor::on_frame() {
        m_input.seat().notify_frame();
    }

    void Cursor::on_swipe_begin(wlr_pointer_swipe_begin_event* event) {
        m_input.seat().notify_activity();
        if (event->fingers == 3 && !m_grab_window && !m_input.seat().is_drag_active() &&
            m_input.compositor().begin_workspace_swipe()) {
            m_input.seat().clear_pointer_focus();
            return;
        }
        wlr_pointer_gestures_v1_send_swipe_begin(m_pointer_gestures, m_input.seat().raw(), event->time_msec,
                                                 event->fingers);
    }

    void Cursor::on_swipe_update(wlr_pointer_swipe_update_event* event) {
        m_input.seat().notify_activity();
        if (m_input.compositor().is_workspace_swiping()) {
            m_input.compositor().update_workspace_swipe(event->dx);
            return;
        }
        wlr_pointer_gestures_v1_send_swipe_update(m_pointer_gestures, m_input.seat().raw(), event->time_msec, event->dx,
                                                  event->dy);
    }

    void Cursor::on_swipe_end(wlr_pointer_swipe_end_event* event) {
        m_input.seat().notify_activity();
        if (m_input.compositor().is_workspace_swiping()) {
            m_input.compositor().end_workspace_swipe();
            return;
        }
        wlr_pointer_gestures_v1_send_swipe_end(m_pointer_gestures, m_input.seat().raw(), event->time_msec,
                                               event->cancelled);
    }

    void Cursor::process_motion(uint32_t time_msec) {
        m_input.seat().update_drag_icons_position();

        if (m_grab_window) {
            const int dx = static_cast<int>(std::round(m_wlr_cursor->x - m_grab_x));
            const int dy = static_cast<int>(std::round(m_wlr_cursor->y - m_grab_y));
            m_grab_window->update_grab(dx, dy);
            return;
        }

        if (m_input.compositor().is_workspace_swiping()) {
            return;
        }

        if (Output* out = m_input.compositor().focused_output(); out && out->animation().is_active()) {
            return;
        }

        if (!m_input.compositor().exclusive_layer()) {
            if (auto* constraint = m_input.seat().active_constraint();
                constraint && constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
                return;
            }
        }

        double sx = 0.0;
        double sy = 0.0;
        wlr_surface* surface = m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy);
        if (surface) {
            if (const auto* excl = m_input.compositor().exclusive_layer()) {
                const auto* layer = Layer::try_from(root_popup_parent(surface));
                if (!layer || !layer->is_mapped() || !layer->is_exclusive() || layer->layer() != excl->layer()) {
                    m_input.seat().clear_pointer_focus();
                    return;
                }
            }
            m_input.seat().notify_enter(surface, sx, sy);
            m_input.seat().notify_motion(time_msec, sx, sy);
        } else {
            m_input.seat().clear_pointer_focus();
        }
    }

    void Cursor::update_focus() {
        if (m_input.compositor().is_workspace_swiping()) {
            return;
        }

        if (m_grab_window) {
            if (m_input.compositor().exclusive_layer() || !m_grab_window->is_mapped() ||
                !m_grab_window->is_floating() || m_grab_window != m_input.compositor().focused_window()) {
                end_grab();
            }
            return;
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto time_msec =
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        process_motion(time_msec);
    }

}
