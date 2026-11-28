#include "frost/layout.hpp"

namespace frost::layout {

    std::unique_ptr<Base> create(Layout mode) {
        switch (mode) {
            case Layout::Master:
                return detail::create_master();
            case Layout::Monocle:
                return detail::create_monocle();
        }
        return detail::create_master();
    }

}
