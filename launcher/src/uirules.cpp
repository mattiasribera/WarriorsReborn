#include "uirules.h"

namespace launcher {

bool play_enabled(bool wizard, bool running, bool ready) { return !wizard && !running && ready; }

bool path_message_visible(bool editing, double now, double last_edit) {
    return !editing || now - last_edit >= PATH_SETTLE_SECONDS - 1e-6; /* 1e-6: 10.6 - 10.0 is 0.5999... */
}

} // namespace launcher
