/* Small window rules kept out of the GUI so launcher_test can pin them: when PLAY is enabled, and
 * when a typed path's "not found" message may show (not on every keystroke while typing). */
#ifndef LAUNCHER_UIRULES_H
#define LAUNCHER_UIRULES_H

namespace launcher {

/* PLAY works only outside the first-run wizard, while no game runs and the game copy is ready.
 * During the wizard it stays off on every step, even once the copy is ready (step 2 onward). */
bool play_enabled(bool wizard, bool running, bool ready);

/* Seconds without typing before a path field being edited shows its validation message. */
constexpr double PATH_SETTLE_SECONDS = 0.6;

/* A path field's validation message shows when the field is not being edited (it lost focus, or
 * was never typed in), or PATH_SETTLE_SECONDS after its last edit. now and last_edit in seconds. */
bool path_message_visible(bool editing, double now, double last_edit);

} // namespace launcher
#endif
