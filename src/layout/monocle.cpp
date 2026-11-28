#include "frost/layout.hpp"
#include "frost/window.hpp"

#include <cstddef>

namespace frost::layout {

    namespace {

        [[nodiscard]] wlr_box cell_box(const Context& ctx) noexcept {
            const WorkArea area = work_area(ctx.usable, ctx.config, true);
            return inset_border(area.box, area.border_width);
        }

        class Monocle final : public Base {
        public:
            [[nodiscard]] Layout mode() const noexcept override { return Layout::Monocle; }
            [[nodiscard]] bool is_solitary(size_t /*tiled_count*/) const noexcept override { return true; }
            [[nodiscard]] bool hides_unfocused() const noexcept override { return true; }
            [[nodiscard]] bool can_resize(size_t /*tiled_count*/) const noexcept override { return false; }

            void arrange(const Context& ctx) override {
                const wlr_box box = cell_box(ctx);
                for (Window* w : ctx.windows) {
                    if (w->is_tiled()) {
                        w->set_geometry(box);
                    }
                }
            }

            [[nodiscard]] wlr_box next_tile_box(const Context& ctx) const noexcept override { return cell_box(ctx); }

            [[nodiscard]] Window* window_in_direction(const Context& ctx, const Window* focused, Direction dir,
                                                      bool has_adj_output) const noexcept override {
                Window* first = nullptr;
                Window* last = nullptr;
                Window* prev = nullptr;
                Window* next = nullptr;
                size_t count = 0;
                size_t index = 0;
                bool found = false;

                for (Window* w : ctx.windows) {
                    if (!w->is_tiled()) {
                        continue;
                    }
                    if (!first) {
                        first = w;
                    }
                    if (w == focused) {
                        found = true;
                        index = count;
                        prev = last;
                    } else if (found && !next) {
                        next = w;
                    }
                    last = w;
                    ++count;
                }

                if (count <= 1 || !found) {
                    return nullptr;
                }

                const bool at_edge = (dir == Direction::Left || dir == Direction::Right) ||
                                     (dir == Direction::Up && index == 0) ||
                                     (dir == Direction::Down && index == count - 1);
                Window* target =
                    (dir == Direction::Down || dir == Direction::Right) ? (next ? next : first) : (prev ? prev : last);
                return (at_edge && has_adj_output) ? nullptr : target;
            }
        };

    }

    namespace detail {

        std::unique_ptr<Base> create_monocle() {
            return std::make_unique<Monocle>();
        }

    }

}
