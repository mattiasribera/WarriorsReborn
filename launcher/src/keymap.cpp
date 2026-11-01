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
};

static const char *const CONTROL_NAMES[CTL_COUNT] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right", "spin_left_stick",
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

bool key_reserved(int key) { return key == KEY_ESCAPE || key == KEY_F1; }
bool key_is_mouse(int key) { return key >= MOUSE_LEFT && key <= MOUSE_X2; }

std::string key_display_name(int key) {
    if (key >= 'a' && key <= 'z') return std::string(1, (char)(key - 32));
    if (key >= '0' && key <= '9') return std::string(1, (char)key);
    for (const auto &n : NAMED)
        if (n.key == key) return n.display;
    return "?";
}

KeyMap::KeyMap() { reset(); }

void KeyMap::reset() {
    static const struct { int key, control; } def[] = {   /* runtime/src/input_map.c defaults() */
        {'w', CTL_LS_UP}, {'s', CTL_LS_DOWN}, {'a', CTL_LS_LEFT}, {'d', CTL_LS_RIGHT},
        {'i', CTL_RS_UP}, {'k', CTL_RS_DOWN}, {'j', CTL_RS_LEFT}, {'l', CTL_RS_RIGHT},
        {KEY_UP, CTL_UP}, {KEY_DOWN, CTL_DOWN}, {KEY_LEFT, CTL_LEFT}, {KEY_RIGHT, CTL_RIGHT},
        {MOUSE_LEFT, CTL_X}, {'r', CTL_X}, {MOUSE_RIGHT, CTL_A}, {' ', CTL_A}, {'f', CTL_B}, {'e', CTL_Y},
        {'q', CTL_LT}, {KEY_LSHIFT, CTL_RT}, {KEY_LCTRL, CTL_BLACK}, {'\t', CTL_WHITE},
        {'c', CTL_LTHUMB}, {MOUSE_MIDDLE, CTL_RTHUMB}, {'v', CTL_RTHUMB},
        {'\r', CTL_START}, {'\b', CTL_BACK}, {'g', CTL_SPIN},
    };
    for (int &b : binding) b = -1;
    for (const auto &d : def) binding[d.key] = d.control;
    settings = {{"mouse.sensitivity", "1.0"}, {"mouse.invert_y", "false"}, {"mouse.deadzone", "0.15"},
                {"mouse.smoothing", "0.05"}, {"spin.rate", "2"}};
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
    for (int k = MOUSE_LEFT; k <= MOUSE_X2; k++)
        if (binding[k] == control) out.push_back(k);
    for (int k = 0; k < KEY_COUNT; k++)
        if (!key_is_mouse(k) && binding[k] == control) out.push_back(k);
    return out;
}

int KeyMap::bind(int key, int control) {
    if (key < 0 || key >= KEY_COUNT || key_reserved(key) || !key_config_name(key) || control < 0 || control >= CTL_COUNT) return -2;
    int prev = binding[key];
    binding[key] = control;
    return prev;
}

bool KeyMap::operator==(const KeyMap &o) const {
    return !memcmp(binding, o.binding, sizeof binding) && settings == o.settings;
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
        if ((name.compare(0, 6, "mouse.") == 0 && key_from_config_name(name) < 0) || name.compare(0, 5, "spin.") == 0) {
            m.set(name, value);   /* a mouse or spin setting */
            continue;
        }
        int key = name.compare(0, 4, "key.") == 0 ? key_from_config_name(name.substr(4)) : key_from_config_name(name);
        if (name.compare(0, 4, "key.") && name.compare(0, 6, "mouse.")) {
            warn("unknown setting '" + name + "'");
            continue;
        }
        if (key < 0) {
            warn("unknown or reserved key '" + name + "'");
            continue;
        }
        int control = value == "none" ? -1 : control_from_config_name(value);
        if (control < 0 && value != "none") {
            warn("unknown pad control '" + value + "'");
            continue;
        }
        m.binding[key] = control;
    }
    return m;
}

std::string write_keymap(const KeyMap &map) {
    std::string out =
        "# The Warriors key map (written by the launcher's Controls page).\n"
        "# key.NAME = CONTROL, mouse.BUTTON = CONTROL; CONTROL is a pad control, spin_left_stick or none.\n";
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
