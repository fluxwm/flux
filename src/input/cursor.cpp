#include "frost/input/cursor.hpp"
#include "frost/compositor.hpp"
#include "frost/input.hpp"
#include "frost/input/grab.hpp"
#include "frost/layer.hpp"
#include "frost/output.hpp"
#include "frost/util.hpp"
#include "frost/window.hpp"
#include "frost/workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>

namespace frost::input {

    namespace {

        [[nodiscard]] Layer* matching_exclusive_layer(wlr_surface* surface, const Layer* excl) noexcept {
            auto* layer = Layer::try_from(root_popup_parent(surface));
            if (layer && layer->is_mapped() && layer->is_exclusive() && layer->layer() == excl->layer()) {
                return layer;
            }
            return nullptr;
        }

        [[nodiscard]] bool clamp_to_region(pixman_region32_t* region, double tx, double ty, double& out_x,
                                           double& out_y) noexcept {
            int nboxes = 0;
            const pixman_box32_t* raw_boxes = pixman_region32_rectangles(region, &nboxes);
            if (!raw_boxes || nboxes <= 0) {
                return false;
            }

            bool found = false;
            double best_dist = std::numeric_limits<double>::infinity();
            for (const auto& b : std::span(raw_boxes, static_cast<size_t>(nboxes))) {
                if (b.x2 <= b.x1 || b.y2 <= b.y1) {
                    continue;
                }
                const double x1 = static_cast<double>(b.x1);
                const double y1 = static_cast<double>(b.y1);
                const double x2 = std::nextafter(static_cast<double>(b.x2), x1);
                const double y2 = std::nextafter(static_cast<double>(b.y2), y1);
                const double cx = std::clamp(tx, x1, x2);
                const double cy = std::clamp(ty, y1, y2);
                const double ddx = tx - cx;
                const double ddy = ty - cy;
                const double dist = (ddx * ddx) + (ddy * ddy);
                if (!found || dist < best_dist) {
                    found = true;
                    best_dist = dist;
                    out_x = cx;
                    out_y = cy;
                }
            }
            return found;
        }

    }

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

        const char* theme = effective_theme();
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

    Window* Cursor::grabbed_window() const noexcept {
        return m_grab ? m_grab->window() : nullptr;
    }

    bool Cursor::is_tiled_resizing(const Workspace* ws) const noexcept {
        return m_grab && m_grab->resized_workspace() == ws;
    }

    void Cursor::clear_grab() noexcept {
        m_grab.reset();
    }

    bool Cursor::resolve_button_grab(int* ox, int* oy) const noexcept {
        if (!m_button_grab || m_input.seat().pointer_button_count() == 0 || m_input.seat().is_drag_active()) {
            return false;
        }
        wlr_surface* surface = m_input.seat().focused_pointer_surface();
        if (!surface || !surface->mapped) {
            return false;
        }
        const Compositor& comp = m_input.compositor();
        wlr_surface* root = root_popup_parent(surface);
        if (comp.is_session_locked()) {
            const Output* lock_out = comp.output_from_lock_surface(root);
            if (!lock_out || !lock_out->is_enabled()) {
                return false;
            }
        } else {
            if (const auto* excl = comp.exclusive_layer(); excl && !matching_exclusive_layer(surface, excl)) {
                return false;
            }
            if (const Window* win = Window::try_from(root)) {
                const Workspace* ws = win->workspace();
                if (!ws || !ws->output().is_enabled() || ws != &ws->output().active_workspace() ||
                    ws->output().animation().is_busy()) {
                    return false;
                }
            } else if (const Layer* layer = Layer::try_from(root)) {
                if (!layer->output() || !layer->output()->is_enabled()) {
                    return false;
                }
            }
        }
        int x = 0;
        int y = 0;
        if (!comp.scene().surface_coords(surface, x, y)) {
            return false;
        }
        if (ox) {
            *ox = x;
        }
        if (oy) {
            *oy = y;
        }
        return true;
    }

    void Cursor::start_grab(Window* window, uint32_t button) {
        if (m_grab) {
            end_grab(false);
        }

        if (button == btn_right) {
            if (window->is_tiled() && window->workspace()) {
                auto session = window->workspace()->begin_tiled_resize(window, m_wlr_cursor->x, m_wlr_cursor->y);
                if (!session) {
                    return;
                }
                m_grab = std::make_unique<TiledResizeGrab>(window, button, m_wlr_cursor->x, m_wlr_cursor->y,
                                                           std::move(session));
            } else if (window->is_floating()) {
                const wlr_box box = window->geometry_box();
                const double rel_x = m_wlr_cursor->x - box.x;
                const double rel_y = m_wlr_cursor->y - box.y;
                uint32_t edges = WLR_EDGE_NONE;

                if (rel_x < box.width / 3.0) {
                    edges |= WLR_EDGE_LEFT;
                } else if (rel_x > box.width * (2.0 / 3.0)) {
                    edges |= WLR_EDGE_RIGHT;
                }

                if (rel_y < box.height / 3.0) {
                    edges |= WLR_EDGE_TOP;
                } else if (rel_y > box.height * (2.0 / 3.0)) {
                    edges |= WLR_EDGE_BOTTOM;
                }

                if (edges == WLR_EDGE_NONE) {
                    const uint32_t horiz = (rel_x < (box.width / 2.0)) ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
                    const uint32_t vert = (rel_y < (box.height / 2.0)) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
                    edges = horiz | vert;
                }

                m_grab = std::make_unique<ResizeGrab>(window, button, m_wlr_cursor->x, m_wlr_cursor->y, edges);
            }
        } else if (window->is_tiled() && window->workspace()) {
            m_grab = std::make_unique<TiledMoveGrab>(window, button, m_wlr_cursor->x, m_wlr_cursor->y);
        } else if (window->is_floating()) {
            m_grab = std::make_unique<MoveGrab>(window, button, m_wlr_cursor->x, m_wlr_cursor->y);
        }

        if (!m_grab) {
            return;
        }

        m_input.seat().clear_pointer_focus();
        set_xcursor(m_grab->cursor_name());
        m_grab->start();
    }

    void Cursor::end_grab(bool commit) {
        if (!m_grab) {
            return;
        }
        auto grab = std::move(m_grab);
        grab->end(commit);
        set_xcursor("default");
        update_focus();
    }

    void Cursor::cancel_grab(const Window* window) {
        if (m_grab && (!window || m_grab->involves_window(window))) {
            end_grab(false);
        }
    }

    void Cursor::toggle_grab_floating() {
        if (!m_grab) {
            return;
        }
        Window* window = m_grab->window();
        if (!window || !window->is_mapped() || window->is_fullscreen()) {
            return;
        }
        Workspace* ws = window->workspace();
        if (!ws) {
            return;
        }

        const wlr_box cur = window->geometry_box();

        if (window->is_tiled()) {
            m_grab.reset();
            window->set_floating_in_grab(true, cur);
            ws->restack();
            ws->arrange();
            m_grab = std::make_unique<MoveGrab>(window, btn_left, m_wlr_cursor->x, m_wlr_cursor->y);
            m_grab->start();
        } else if (window->is_floating()) {
            m_grab.reset();
            window->set_floating_in_grab(false, cur);
            ws->restack();
            m_grab = std::make_unique<TiledMoveGrab>(window, btn_left, m_wlr_cursor->x, m_wlr_cursor->y);
            m_grab->start();
            ws->arrange();
            ws->swap_tiled_at(window, m_wlr_cursor->x, m_wlr_cursor->y);
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
        int ox = 0;
        int oy = 0;
        if (!m_input.compositor().scene().surface_coords(constraint->surface, ox, oy)) {
            return true;
        }
        const double sx = m_wlr_cursor->x - ox;
        const double sy = m_wlr_cursor->y - oy;

        pixman_region32_t* region =
            pixman_region32_not_empty(&constraint->region) || pixman_region32_not_empty(&constraint->current.region)
                ? &constraint->region
                : &constraint->surface->input_region;
        double confined_x = 0.0;
        double confined_y = 0.0;
        if (!wlr_region_confine(region, sx, sy, sx + dx, sy + dy, &confined_x, &confined_y) &&
            !clamp_to_region(region, sx + dx, sy + dy, confined_x, confined_y)) {
            return false;
        }

        double next_dx = (ox + confined_x) - m_wlr_cursor->x;
        double next_dy = (oy + confined_y) - m_wlr_cursor->y;
        auto contains = [&](double test_dx, double test_dy) noexcept {
            const double next_sx = (m_wlr_cursor->x + test_dx) - ox;
            const double next_sy = (m_wlr_cursor->y + test_dy) - oy;
            return pixman_region32_contains_point(region, static_cast<int>(std::floor(next_sx)),
                                                  static_cast<int>(std::floor(next_sy)), nullptr) != 0;
        };
        if (!contains(next_dx, next_dy)) {
            constexpr double neg_inf = -std::numeric_limits<double>::infinity();
            const double prev_dx =
                std::min(std::nextafter(ox + confined_x, neg_inf) - m_wlr_cursor->x, std::nextafter(next_dx, neg_inf));
            const double prev_dy =
                std::min(std::nextafter(oy + confined_y, neg_inf) - m_wlr_cursor->y, std::nextafter(next_dy, neg_inf));
            if (contains(prev_dx, next_dy)) {
                next_dx = prev_dx;
            } else if (contains(next_dx, prev_dy)) {
                next_dy = prev_dy;
            } else if (contains(prev_dx, prev_dy)) {
                next_dx = prev_dx;
                next_dy = prev_dy;
            }
        }

        dx = next_dx;
        dy = next_dy;
        return true;
    }

    void Cursor::handle_motion(wlr_input_device* device, uint32_t time_msec, double dx, double dy, double unaccel_dx,
                               double unaccel_dy) {
        m_input.seat().notify_activity();
        wlr_relative_pointer_manager_v1_send_relative_motion(m_relative_pointer_mgr, m_input.seat().raw(),
                                                             static_cast<uint64_t>(time_msec) * 1000, dx, dy,
                                                             unaccel_dx, unaccel_dy);
        if (apply_constraint(dx, dy)) {
            wlr_output* prev_out =
                wlr_output_layout_output_at(m_input.compositor().output_layout(), m_wlr_cursor->x, m_wlr_cursor->y);
            wlr_cursor_move(m_wlr_cursor, device, dx, dy);
            wlr_output* next_out =
                wlr_output_layout_output_at(m_input.compositor().output_layout(), m_wlr_cursor->x, m_wlr_cursor->y);
            if (next_out != prev_out && !m_grab && !m_button_grab && !m_input.compositor().focused_window() &&
                !m_input.compositor().is_locked()) {
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
        if (m_grab) {
            m_input.seat().notify_activity();
            if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && event->button == m_grab->button()) {
                end_grab();
                return;
            }
            if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == btn_right &&
                m_grab->button() == btn_left) {
                toggle_grab_floating();
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

        if (m_button_grab) {
            const bool valid = resolve_button_grab();
            if (valid || event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
                m_input.seat().notify_button(event->time_msec, event->button, event->state);
                if (!valid || m_input.seat().pointer_button_count() == 0) {
                    clear_button_grab();
                    update_focus();
                }
                return;
            }
            clear_button_grab();
            update_focus();
        }

        if (m_input.compositor().is_session_locked()) {
            if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
                double sx = 0.0;
                double sy = 0.0;
                if (wlr_surface* surface =
                        m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy)) {
                    if (auto* out = m_input.compositor().output_from_lock_surface(surface)) {
                        m_input.compositor().set_focused_output(out);
                        m_input.compositor().focus_surface(out->lock_surface()->surface);
                    }
                }
            }
        } else if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            const auto* excl = m_input.compositor().exclusive_layer();
            if (!excl) {
                m_input.compositor().set_focused_output(nullptr);
                if (Output* out = m_input.compositor().focused_output(); out && out->animation().is_active()) {
                    out->animation().cancel();
                    m_input.compositor().on_animation_finished();
                }
            }

            double sx = 0.0;
            double sy = 0.0;
            wlr_surface* surface = nullptr;
            Window* window =
                m_input.compositor().scene().window_at(m_wlr_cursor->x, m_wlr_cursor->y, &surface, &sx, &sy);
            if (excl) {
                if (const auto* layer = matching_exclusive_layer(surface, excl)) {
                    m_input.compositor().set_focused_output(layer->output());
                    m_input.compositor().focus_surface(layer->surface());
                }
            } else if (window != nullptr) {
                m_input.compositor().focus_window(window);

                wlr_keyboard* keyboard = m_input.seat().keyboard();
                const uint32_t mods = keyboard ? (wlr_keyboard_get_modifiers(keyboard) & key_modifier_mask) : 0;
                const bool can_tiled_resize = event->button == btn_right && window->is_tiled() && window->workspace() &&
                                              window->workspace()->can_tiled_resize();
                const bool can_tiled_move = event->button == btn_left && window->is_tiled() && window->workspace();
                if (mods == m_input.compositor().mod_key() && window == m_input.compositor().focused_window() &&
                    (event->button == btn_left || event->button == btn_right) &&
                    (window->is_floating() || can_tiled_resize || can_tiled_move)) {
                    m_input.seat().notify_activity();
                    start_grab(window, event->button);
                    return;
                }
            } else if (surface != nullptr) {
                if (auto* layer = Layer::try_from(root_popup_parent(surface)); layer && layer->is_focusable()) {
                    m_input.compositor().set_focused_output(layer->output());
                    m_input.compositor().focus_surface(layer->surface());
                } else if (auto* xsurf = wlr_xwayland_surface_try_from_wlr_surface(surface);
                           xsurf && xsurf->override_redirect &&
                           wlr_xwayland_surface_override_redirect_wants_focus(xsurf)) {
                    m_input.compositor().set_focused_output(
                        m_input.compositor().output_at(m_wlr_cursor->x, m_wlr_cursor->y));
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
        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && m_input.seat().pointer_button_count() > 0 &&
            m_input.seat().focused_pointer_surface() && !m_input.seat().is_drag_active()) {
            m_button_grab = true;
        }
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
        if (m_swiping_workspace) {
            return;
        }
        if (m_button_grab && !resolve_button_grab()) {
            clear_button_grab();
        }
        if (event->fingers == 3 && !m_grab && !m_button_grab && !m_input.seat().is_drag_active() &&
            m_input.compositor().begin_workspace_swipe()) {
            m_swiping_workspace = true;
            m_input.seat().clear_pointer_focus();
            return;
        }
        wlr_pointer_gestures_v1_send_swipe_begin(m_pointer_gestures, m_input.seat().raw(), event->time_msec,
                                                 event->fingers);
    }

    void Cursor::on_swipe_update(wlr_pointer_swipe_update_event* event) {
        m_input.seat().notify_activity();
        if (m_swiping_workspace) {
            m_input.compositor().update_workspace_swipe(event->dx);
            return;
        }
        wlr_pointer_gestures_v1_send_swipe_update(m_pointer_gestures, m_input.seat().raw(), event->time_msec, event->dx,
                                                  event->dy);
    }

    void Cursor::on_swipe_end(wlr_pointer_swipe_end_event* event) {
        m_input.seat().notify_activity();
        if (m_swiping_workspace) {
            m_swiping_workspace = false;
            m_input.compositor().end_workspace_swipe();
            return;
        }
        wlr_pointer_gestures_v1_send_swipe_end(m_pointer_gestures, m_input.seat().raw(), event->time_msec,
                                               event->cancelled);
    }

    void Cursor::process_motion(uint32_t time_msec) {
        m_input.seat().update_drag_icons_position();

        if (m_grab) {
            m_grab->on_motion(m_wlr_cursor->x, m_wlr_cursor->y);
            return;
        }

        if (m_input.compositor().is_workspace_swiping()) {
            return;
        }

        if (const auto* constraint = m_input.seat().active_constraint();
            constraint && constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED && !m_input.compositor().is_locked()) {
            return;
        }

        if (m_button_grab) {
            int ox = 0;
            int oy = 0;
            if (resolve_button_grab(&ox, &oy)) {
                m_input.seat().notify_motion(time_msec, m_wlr_cursor->x - ox, m_wlr_cursor->y - oy);
                return;
            }
            clear_button_grab();
        }

        double sx = 0.0;
        double sy = 0.0;
        wlr_surface* surface = m_input.compositor().scene().surface_at(m_wlr_cursor->x, m_wlr_cursor->y, &sx, &sy);
        if (surface) {
            if (!m_input.compositor().is_session_locked()) {
                if (const auto* excl = m_input.compositor().exclusive_layer();
                    excl && !matching_exclusive_layer(surface, excl)) {
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

        if (m_grab) {
            if (!m_grab->is_valid()) {
                end_grab(false);
            }
            return;
        }

        wlr_output* wlr_out =
            wlr_output_layout_output_at(m_input.compositor().output_layout(), m_wlr_cursor->x, m_wlr_cursor->y);
        if (Output* out = m_input.compositor().output_from_wlr(wlr_out); out && out->animation().is_active()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto time_msec =
            static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
        process_motion(time_msec);
    }

}
