#include "keymap.h"
#include "platform.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace launcher {

static const struct { int key; const char *name, *display; } NAMED[] = {
    {' ', "space", "Space"}, {'\r', "enter", "Enter"}, {'\b', "backspace", "Backspace"}, {'\t', "tab", "Tab"},
    {KEY_UP, "up", "Up"}, {KEY_DOWN, "down", "Down"}, {KEY_LEFT, "left", "Left"}, {KEY_RIGHT, "right", "Right"},
    {KEY_LSHIFT, "lshift", "Shift"}, {KEY_RSHIFT, "rshift", "Right Shift"}, {KEY_LCTRL, "lctrl", "Ctrl"},
    {KEY_RCTRL, "rctrl", "Right Ctrl"}, {KEY_LALT, "lalt", "Alt"}, {KEY_RALT, "ralt", "Right Alt"},
    {KEY_F1 + 1, "f2", "F2"}, {KEY_F1 + 2, "f3", "F3"}, {KEY_F1 + 3, "f4", "F4"}, {KEY_F1 + 4, "f5", "F5"},
    {KEY_F1 + 5, "f6", "F6"}, {KEY_F1 + 6, "f7", "F7"}, {KEY_F1 + 7, "f8", "F8"}, {KEY_F1 + 8, "f9", "F9"},
    {KEY_F1 + 9, "f10", "F10"}, {KEY_F1 + 10, "f11", "F11"}, {KEY_F1 + 11, "f12", "F12"},
    {'-', "minus", "-"}, {'=', "equals", "="}, {'[', "lbracket", "["}, {']', "rbracket", "]"}, {'\\', "backslash", "\\"},
    {';', "semicolon", ";"}, {'\'', "apostrophe", "'"}, {'`', "grave", "`"}, {',', "comma", ","}, {'.', "period", "."},
    {'/', "slash", "/"},
    {MOUSE_LEFT, "mouse.left", "Mouse left"}, {MOUSE_MIDDLE, "mouse.middle", "Mouse middle"},
    {MOUSE_RIGHT, "mouse.right", "Mouse right"}, {MOUSE_X1, "mouse.x1", "Mouse back"}, {MOUSE_X2, "mouse.x2", "Mouse forward"},
    {MOUSE_WHEEL, "mouse.wheel", "Mouse wheel"},
};

static const char *const CONTROL_NAMES[CTL_COUNT] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right", "spin_left_stick", "mash_a",
};

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

const char *key_config_name(int key) {
    static char single[128][2];
    if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9')) {
        single[key][0] = (char)key;
        return single[key];
    }
    for (const auto &n : NAMED)
        if (n.key == key) return n.name;
    return nullptr;
}

int key_from_config_name(const std::string &name) {
    std::string n = lower(name);
    if (n.size() == 1 && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    for (const auto &k : NAMED)
        if (n == k.name) return k.key;
    return -1;
}

const char *control_config_name(int control) { return control >= 0 && control < CTL_COUNT ? CONTROL_NAMES[control] : nullptr; }

int control_from_config_name(const std::string &name) {
    std::string n = lower(name);
    for (int c = 0; c < CTL_COUNT; c++)
        if (n == CONTROL_NAMES[c]) return c;
    static const struct { const char *alias; int control; } aliases[] = {   /* the runtime's short names */
        {"up", CTL_UP}, {"down", CTL_DOWN}, {"left", CTL_LEFT}, {"right", CTL_RIGHT}, {"lthumb", CTL_LTHUMB},
        {"rthumb", CTL_RTHUMB}, {"lt", CTL_LT}, {"rt", CTL_RT}, {"lsup", CTL_LS_UP}, {"lsdown", CTL_LS_DOWN},
        {"lsleft", CTL_LS_LEFT}, {"lsright", CTL_LS_RIGHT}, {"rsup", CTL_RS_UP}, {"rsdown", CTL_RS_DOWN},
        {"rsleft", CTL_RS_LEFT}, {"rsright", CTL_RS_RIGHT},
    };
    for (const auto &a : aliases)
        if (n == a.alias) return a.control;
    return -1;
}

bool key_reserved(int key) { return key == KEY_ESCAPE || key == KEY_F1 || key == KEY_F10; }
bool key_is_mouse(int key) { return key >= MOUSE_LEFT && key <= MOUSE_WHEEL; }

std::string key_display_name(int key) {
    if (key >= 'a' && key <= 'z') return std::string(1, (char)(key - 32));
    if (key >= '0' && key <= '9') return std::string(1, (char)key);
    for (const auto &n : NAMED)
        if (n.key == key) return n.display;
    return "?";
}

KeyMap::KeyMap() { reset(); }

void KeyMap::reset() {
    reset_keys();
    pad_reset();
}

void KeyMap::pad_reset() {
    for (int b = 0; b < PAD_BUTTONS; b++) pad[b] = b;
    swap_sticks = false;
}

bool KeyMap::pad_remapped() const {
    for (int b = 0; b < PAD_BUTTONS; b++)
        if (pad[b] != b) return true;
    return swap_sticks;
}

int KeyMap::pad_button_for(int control) const {
    if (control < 0 || control >= PAD_BUTTONS) return -1;
    if (pad[control] == control) return control;
    for (int b = 0; b < PAD_BUTTONS; b++)
        if (pad[b] == control) return b;
    return -1;
}

int KeyMap::pad_assign(int control, int button) {
    if (control < 0 || control >= PAD_BUTTONS || button < 0 || button >= PAD_BUTTONS) return -2;
    int prev = pad[button], other = pad_button_for(control);
    pad[button] = control;
    if (other >= 0 && other != button) pad[other] = prev;
    return prev;
}

int pad_input_pressed(const bool button[15], const float axis[6]) {
    static const int controls[15] = {   /* A B X Y BACK GUIDE START LSTICK RSTICK LB RB UP DOWN LEFT RIGHT */
        CTL_A, CTL_B, CTL_X, CTL_Y, CTL_BACK, -1, CTL_START, CTL_LTHUMB, CTL_RTHUMB, CTL_WHITE, CTL_BLACK,
        CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT};
    for (int b = 0; b < 15; b++)
        if (button[b] && controls[b] >= 0) return controls[b];
    if (axis[4] > 0.5f) return CTL_LT;
    if (axis[5] > 0.5f) return CTL_RT;
    return -1;
}

void KeyMap::reset_keys() {
    static const struct { int key, control; } def[] = {   /* runtime/src/input_map.c defaults() */
        {'w', CTL_LS_UP}, {'s', CTL_LS_DOWN}, {'a', CTL_LS_LEFT}, {'d', CTL_LS_RIGHT},
        {'i', CTL_RS_UP}, {'k', CTL_RS_DOWN}, {'j', CTL_RS_LEFT}, {'l', CTL_RS_RIGHT},
        {KEY_UP, CTL_UP}, {KEY_DOWN, CTL_DOWN}, {KEY_LEFT, CTL_LEFT}, {KEY_RIGHT, CTL_RIGHT},
        {MOUSE_LEFT, CTL_X}, {'r', CTL_X}, {MOUSE_RIGHT, CTL_A}, {' ', CTL_A}, {'f', CTL_B}, {'e', CTL_Y},
        {'q', CTL_LT}, {KEY_LSHIFT, CTL_RT}, {KEY_LCTRL, CTL_BLACK}, {'\t', CTL_WHITE},
        {'c', CTL_LTHUMB}, {MOUSE_MIDDLE, CTL_RTHUMB}, {'v', CTL_RTHUMB},
        {'\r', CTL_START}, {'\b', CTL_BACK}, {'g', CTL_SPIN}, {'h', CTL_MASH}, {MOUSE_WHEEL, CTL_MASH},
    };
    for (int &b : binding) b = -1;
    for (const auto &d : def) binding[d.key] = d.control;
    settings = {{"mouse.sensitivity", "2.0"}, {"mouse.sensitivity_x", "2.0"}, {"mouse.sensitivity_y", "2.0"},
                {"mouse.invert_y", "false"}, {"mouse.deadzone", "0.15"}, {"mouse.smoothing", "0.05"},
                {"spin.rate", "2"}, {"mash.rate", "12"}};
}

void KeyMap::set(const std::string &name, const std::string &value) {
    for (auto &s : settings)
        if (s.first == name) {
            s.second = value;
            return;
        }
    settings.push_back({name, value});
}

std::string KeyMap::get(const std::string &name) const {
    for (const auto &s : settings)
        if (s.first == name) return s.second;
    return "";
}

std::vector<int> KeyMap::keys_for(int control) const {
    std::vector<int> out;
    for (int k = MOUSE_LEFT; k <= MOUSE_WHEEL; k++)
        if (binding[k] == control) out.push_back(k);
    for (int k = 0; k < KEY_COUNT; k++)
        if (!key_is_mouse(k) && binding[k] == control) out.push_back(k);
    return out;
}

int KeyMap::bind(int key, int control) {
    if (key < 0 || key >= KEY_COUNT || key_reserved(key) || !key_config_name(key) || control < 0 || control >= CTL_COUNT ||
        (key == MOUSE_WHEEL && control == CTL_SPIN)) return -2;   /* the runtime refuses the spin on the wheel */
    int prev = binding[key];
    binding[key] = control;
    return prev;
}

bool KeyMap::operator==(const KeyMap &o) const {
    return !memcmp(binding, o.binding, sizeof binding) && settings == o.settings && !memcmp(pad, o.pad, sizeof pad) &&
           swap_sticks == o.swap_sticks;
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

KeyMap parse_keymap(const std::string &text, std::vector<std::string> *warnings) {
    KeyMap m;
    size_t pos = 0;
    int line_no = 0;
    auto warn = [&](const std::string &why) {
        if (warnings) warnings->push_back("line " + std::to_string(line_no) + ": " + why);
    };
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        line_no++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warn("expected NAME = VALUE");
            continue;
        }
        std::string name = lower(trim(line.substr(0, eq))), value = lower(trim(line.substr(eq + 1)));
        if (name == "pad.swap_sticks") {
            bool on = value == "true" || value == "yes" || value == "on" || value == "1";
            if (!on && value != "false" && value != "no" && value != "off" && value != "0") warn("pad.swap_sticks must be true or false");
            else m.swap_sticks = on;
            continue;
        }
        if (name.compare(0, 4, "pad.") == 0) {
            int b = control_from_config_name(name.substr(4)), c = value == "none" ? -1 : control_from_config_name(value);
            if (b < 0 || b >= PAD_BUTTONS) warn("unknown controller button '" + name.substr(4) + "'");
            else if ((c < 0 && value != "none") || c >= PAD_BUTTONS) warn("'" + value + "' is not a controller button or none");
            else m.pad[b] = c;
            continue;
        }
        if ((name.compare(0, 6, "mouse.") == 0 && key_from_config_name(name) < 0) || name.compare(0, 5, "spin.") == 0 ||
            name.compare(0, 5, "mash.") == 0) {
            m.set(name, value);   /* a mouse, spin or mash setting */
            continue;
        }
        int key = name.compare(0, 4, "key.") == 0 ? key_from_config_name(name.substr(4)) : key_from_config_name(name);
        if (name.compare(0, 4, "key.") && name.compare(0, 6, "mouse.")) {
            warn("unknown setting '" + name + "'");
            continue;
        }
        if (key < 0 || key_reserved(key)) {
            warn("unknown or reserved key '" + name + "'");
            continue;
        }
        int control = value == "none" ? -1 : control_from_config_name(value);
        if (control < 0 && value != "none") {
            warn("unknown pad control '" + value + "'");
            continue;
        }
        if (key == MOUSE_WHEEL && control == CTL_SPIN) {
            warn("the mouse wheel taps a pad control or mash_a, not the spin");
            continue;
        }
        m.binding[key] = control;
    }
    return m;
}

std::string write_keymap(const KeyMap &map) {
    std::string out =
        "# The Warriors key map (written by the launcher's Controls page).\n"
        "# key.NAME = CONTROL, mouse.BUTTON = CONTROL; CONTROL is a pad control, spin_left_stick, mash_a or none.\n"
        "# pad.BUTTON = CONTROL: the controller button BUTTON does CONTROL (only changed buttons are listed).\n";
    for (const auto &s : map.settings) out += s.first + " = " + s.second + "\n";
    KeyMap defaults;
    for (int c = 0; c < CTL_COUNT; c++)
        for (int k : map.keys_for(c)) {
            const char *n = key_config_name(k);
            out += (key_is_mouse(k) ? std::string(n) : "key." + std::string(n)) + " = " + CONTROL_NAMES[c] + "\n";
        }
    for (int k = 0; k < KEY_COUNT; k++)
        if (defaults.binding[k] >= 0 && map.binding[k] < 0) {
            const char *n = key_config_name(k);
            out += (key_is_mouse(k) ? std::string(n) : "key." + std::string(n)) + " = none\n";
        }
    for (int b = 0; b < PAD_BUTTONS; b++)
        if (map.pad[b] != b) out += "pad." + std::string(CONTROL_NAMES[b]) + " = " + (map.pad[b] < 0 ? "none" : CONTROL_NAMES[map.pad[b]]) + "\n";
    if (map.swap_sticks) out += "pad.swap_sticks = true\n";
    return out;
}

bool load_keymap(const std::string &path, KeyMap &out, std::vector<std::string> *warnings) {
    std::string text;
    if (!read_file(path, text, 1u << 20)) return false;
    out = parse_keymap(text, warnings);
    return true;
}

bool save_keymap(const std::string &path, const KeyMap &map, std::string *err) {
    if (!write_file(path, write_keymap(map))) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace launcher
