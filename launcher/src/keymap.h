/* The runtime's keyboard/mouse key map (runtime/src/input_map.h, the --input-config file,
 * warriors-input.cfg by default) as the Controls page edits it.
 *
 * File format (the runtime's): `key.NAME = CONTROL` and `mouse.BUTTON = CONTROL` lines (CONTROL a
 * pad control name, spin_left_stick or none), mouse and spin settings (`mouse.sensitivity = 1`),
 * `#` comments, case-insensitive names. The lines change the runtime's built-in PC layout, so a
 * file only needs the differences. The model keeps every binding (defaults + file) and the
 * settings lines verbatim; write_keymap writes the whole layout (every bound key, plus `none` for
 * each default key left unbound), so the runtime ends with exactly this map whatever its own
 * defaults. Key codes are the runtime's (lower-case ASCII, KEY_*, MOUSE_*). */
#ifndef LAUNCHER_KEYMAP_H
#define LAUNCHER_KEYMAP_H
#include <string>
#include <utility>
#include <vector>

namespace launcher {

enum {
    KEY_ESCAPE = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_F1 = 0x110, /* F1..F12; F1 and Escape are reserved by the runtime */
    MOUSE_LEFT = 0x120, MOUSE_MIDDLE, MOUSE_RIGHT, MOUSE_X1, MOUSE_X2,
    KEY_COUNT = 0x130
};

/* Pad controls in the runtime's order (hle_input.h IN_*), then the spin. */
enum {
    CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT, CTL_START, CTL_BACK, CTL_LTHUMB, CTL_RTHUMB,
    CTL_A, CTL_B, CTL_X, CTL_Y, CTL_BLACK, CTL_WHITE, CTL_LT, CTL_RT,
    CTL_LS_UP, CTL_LS_DOWN, CTL_LS_LEFT, CTL_LS_RIGHT, CTL_RS_UP, CTL_RS_DOWN, CTL_RS_LEFT, CTL_RS_RIGHT,
    CTL_SPIN, CTL_COUNT
};

const char *key_config_name(int key);         /* "w", "lshift", "mouse.left"; nullptr when unknown */
int key_from_config_name(const std::string &name);   /* inverse (case-insensitive), -1 when unknown */
const char *control_config_name(int control); /* "a", "left_stick_up", "spin_left_stick" */
int control_from_config_name(const std::string &name);
bool key_reserved(int key);                   /* Escape and F1 (mouse capture, quit) */
bool key_is_mouse(int key);
/* What a key cap shows: "E", "Shift", "Space", "F5", "Up"...; "Mouse left" etc. for mouse buttons. */
std::string key_display_name(int key);

struct KeyMap {
    int binding[KEY_COUNT];            /* control of each key, -1 = none */
    /* mouse.* and spin.* settings as {name, value}: the runtime's defaults (mouse.sensitivity 1.0,
     * invert_y false, deadzone 0.15, smoothing 0.05, spin.rate 2), a file's value replacing the
     * one of the same name, unknown names appended. Values are kept as written (the runtime
     * checks them). */
    std::vector<std::pair<std::string, std::string>> settings;
    KeyMap();                          /* the runtime's built-in PC layout */
    void reset();                      /* back to it */
    void set(const std::string &name, const std::string &value);
    std::string get(const std::string &name) const;   /* empty when absent */
    std::vector<int> keys_for(int control) const;   /* mouse buttons first, then by key code */
    /* Binds key to control (moving it off the control it had); returns that previous control or
     * -1. A reserved or unknown key is refused (returns -2, nothing changes). */
    int bind(int key, int control);
    void unbind(int key) { if (key >= 0 && key < KEY_COUNT) binding[key] = -1; }
    bool operator==(const KeyMap &o) const;
};

/* Applies the file text on top of the defaults (like the runtime). Invalid lines go to
 * *warnings ("line N: why") and are skipped. */
KeyMap parse_keymap(const std::string &text, std::vector<std::string> *warnings);
std::string write_keymap(const KeyMap &map);
bool load_keymap(const std::string &path, KeyMap &out, std::vector<std::string> *warnings); /* false: unreadable */
bool save_keymap(const std::string &path, const KeyMap &map, std::string *err);

} // namespace launcher
#endif
