/* Platform-neutral host input layer (see input_map.h). */
#include "input_map.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int8_t bindings[INPUT_KEY_COUNT];
static int bindings_ready;
static uint8_t held[INPUT_KEY_COUNT];

/* PC layout (README "Input" has the table and the reasoning). The tutorial strings name the
 * face-button actions: light attack -> X (left mouse), heavy attack / charge / select -> A
 * (right mouse, Space), grab -> B (F), pick up / climb -> Y (E). */
static void defaults(void) {
    static const struct { int key; int8_t control; } def[] = {
        {'w', IN_LS_UP}, {'s', IN_LS_DOWN}, {'a', IN_LS_LEFT}, {'d', IN_LS_RIGHT},
        {'i', IN_RS_UP}, {'k', IN_RS_DOWN}, {'j', IN_RS_LEFT}, {'l', IN_RS_RIGHT},
        {INPUT_KEY_UP, IN_UP}, {INPUT_KEY_DOWN, IN_DOWN}, {INPUT_KEY_LEFT, IN_LEFT}, {INPUT_KEY_RIGHT, IN_RIGHT},
        {INPUT_MOUSE_LEFT, IN_X}, {'r', IN_X}, {INPUT_MOUSE_RIGHT, IN_A}, {' ', IN_A}, {'f', IN_B}, {'e', IN_Y},
        {'q', IN_LT}, {INPUT_KEY_LSHIFT, IN_RT}, {INPUT_KEY_LCTRL, IN_BLACK}, {'\t', IN_WHITE},
        {'c', IN_LTHUMB}, {INPUT_MOUSE_MIDDLE, IN_RTHUMB}, {'v', IN_RTHUMB},
        {'\r', IN_START}, {'\b', IN_BACK}, {'g', INPUT_CTL_SPIN},
    };
    memset(bindings, -1, sizeof bindings);
    for (size_t i = 0; i < sizeof def / sizeof *def; i++) bindings[def[i].key] = def[i].control;
    bindings_ready = 1;
}

int input_key_control(int key) {
    if (!bindings_ready) defaults();
    return key >= 0 && key < INPUT_KEY_COUNT ? bindings[key] : -1;
}
int input_key_for_control(int control) {
    if (control < 0) return -1;
    for (int k = INPUT_MOUSE_LEFT; k <= INPUT_MOUSE_X2; k++)
        if (input_key_control(k) == control) return k;
    for (int k = 0; k < INPUT_KEY_COUNT; k++)
        if (input_key_control(k) == control) return k;
    return -1;
}
int input_key_down(int key) { return key >= 0 && key < INPUT_KEY_COUNT && held[key]; }

/* The pad mask is recomputed from every held key, so two keys bound to one control (Space and
 * Z for A) release it only when both are up; the same holds for the spin key. */
static void publish(void) {
    uint32_t mask = 0;
    int spin = 0;
    for (int k = 0; k < INPUT_KEY_COUNT; k++) {
        int c = held[k] ? input_key_control(k) : -1;
        if (c == INPUT_CTL_SPIN) spin = 1;
        else if (c >= 0) mask |= 1u << c;
    }
    input_set_mask(INPUT_SRC_KEYBOARD, mask);
    input_set_spin(spin);
}

void input_key_event(int key, int down, int repeat) {
    if (repeat || key < 0 || key >= INPUT_KEY_COUNT || held[key] == !!down) return;
    held[key] = !!down;
    if (down) input_note_device(INPUT_DEVICE_KBM);
    publish();
}
void input_release_all(void) {
    memset(held, 0, sizeof held);
    publish();
}

int input_is_repeat_release(const RawKeyEvent *rel, const RawKeyEvent *next) {
    return rel && !rel->press && next && next->press && next->code == rel->code && next->time == rel->time;
}

void input_map_defaults(void) {
    defaults();
    input_mouse_cfg = (MouseCfg){1.0f, 0.15f, 0.05f, 0};
    input_spin_rate = INPUT_SPIN_RATE_DEFAULT;
}

/* ---- names and the key map config ---- */
static const struct { int key; const char *name; } key_names[] = {
    {' ', "space"}, {'\r', "enter"}, {'\b', "backspace"}, {'\t', "tab"},
    {INPUT_KEY_UP, "up"}, {INPUT_KEY_DOWN, "down"}, {INPUT_KEY_LEFT, "left"}, {INPUT_KEY_RIGHT, "right"},
    {INPUT_KEY_LSHIFT, "lshift"}, {INPUT_KEY_RSHIFT, "rshift"}, {INPUT_KEY_LCTRL, "lctrl"}, {INPUT_KEY_RCTRL, "rctrl"},
    {INPUT_KEY_LALT, "lalt"}, {INPUT_KEY_RALT, "ralt"},
    {INPUT_KEY_F1 + 1, "f2"}, {INPUT_KEY_F1 + 2, "f3"}, {INPUT_KEY_F1 + 3, "f4"}, {INPUT_KEY_F1 + 4, "f5"},
    {INPUT_KEY_F1 + 5, "f6"}, {INPUT_KEY_F1 + 6, "f7"}, {INPUT_KEY_F1 + 7, "f8"}, {INPUT_KEY_F1 + 8, "f9"},
    {INPUT_KEY_F1 + 9, "f10"}, {INPUT_KEY_F1 + 10, "f11"}, {INPUT_KEY_F1 + 11, "f12"},
    {'-', "minus"}, {'=', "equals"}, {'[', "lbracket"}, {']', "rbracket"}, {'\\', "backslash"}, {';', "semicolon"},
    {'\'', "apostrophe"}, {'`', "grave"}, {',', "comma"}, {'.', "period"}, {'/', "slash"},
    {INPUT_MOUSE_LEFT, "mouse.left"}, {INPUT_MOUSE_MIDDLE, "mouse.middle"}, {INPUT_MOUSE_RIGHT, "mouse.right"},
    {INPUT_MOUSE_X1, "mouse.x1"}, {INPUT_MOUSE_X2, "mouse.x2"},
};
#define NKEY_NAMES ((int)(sizeof key_names / sizeof *key_names))

const char *input_key_name(int key) {
    static char single[INPUT_KEY_COUNT][2];
    if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9')) {
        single[key][0] = (char)key;
        return single[key];
    }
    for (int i = 0; i < NKEY_NAMES; i++) if (key_names[i].key == key) return key_names[i].name;
    return NULL;
}
int input_key_from_name(const char *name) {
    if (name[0] && !name[1] && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    for (int i = 0; i < NKEY_NAMES; i++) if (!strcmp(key_names[i].name, name)) return key_names[i].key;
    return -1;
}

/* Config names of the pad controls; the --input-script names (input_control_name) work too. */
static const char *const control_long[IN_COUNT] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};
static int control_from_name(const char *name) {
    if (!strcmp(name, "spin_left_stick")) return INPUT_CTL_SPIN;
    for (int c = 0; c < IN_COUNT; c++)
        if (!strcmp(name, control_long[c]) || !strcmp(name, input_control_name(c))) return c;
    return -1;
}

static int parse_float(const char *v, float lo, float hi, float *out) {
    char *end;
    double d = strtod(v, &end);
    if (end == v || *end || !(d >= lo && d <= hi)) return -1;
    *out = (float)d;
    return 0;
}

/* One trimmed, lowercased, comment-free line; returns NULL when valid, else the reason. */
static const char *parse_line(char *line, char *why, size_t whylen) {
    char *eq = strchr(line, '=');
    if (!eq) return "expected NAME = VALUE";
    char *name = line, *val = eq + 1, *e = eq;
    while (e > name && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *e = 0;
    while (*val == ' ' || *val == '\t') val++;
    if (!*name || !*val) return "expected NAME = VALUE";
    float f;
    if (!strcmp(name, "mouse.sensitivity")) {
        if (parse_float(val, 0.01f, 100, &f)) return "mouse.sensitivity must be a number from 0.01 to 100";
        input_mouse_cfg.sensitivity = f;
        return NULL;
    }
    if (!strcmp(name, "mouse.deadzone")) {
        if (parse_float(val, 0, 0.9f, &f)) return "mouse.deadzone must be a number from 0 to 0.9";
        input_mouse_cfg.deadzone = f;
        return NULL;
    }
    if (!strcmp(name, "mouse.smoothing")) {
        if (parse_float(val, 0, 1, &f)) return "mouse.smoothing must be a number of seconds from 0 to 1";
        input_mouse_cfg.smoothing = f;
        return NULL;
    }
    if (!strcmp(name, "mouse.invert_y")) {
        int on = !strcmp(val, "true") || !strcmp(val, "yes") || !strcmp(val, "on") || !strcmp(val, "1");
        if (!on && strcmp(val, "false") && strcmp(val, "no") && strcmp(val, "off") && strcmp(val, "0"))
            return "mouse.invert_y must be true or false";
        input_mouse_cfg.invert_y = on;
        return NULL;
    }
    if (!strcmp(name, "spin.rate")) {
        if (parse_float(val, 0.25f, 10, &f)) return "spin.rate must be a number of turns per second from 0.25 to 10";
        input_spin_rate = f;
        return NULL;
    }
    int key;
    if (!strncmp(name, "key.", 4)) {
        if (!strcmp(name + 4, "escape") || !strcmp(name + 4, "f1")) return "escape and f1 are reserved (mouse capture, quit)";
        key = input_key_from_name(name + 4);
        if (key >= INPUT_MOUSE_LEFT) key = -1;
    } else if (!strncmp(name, "mouse.", 6)) {
        key = input_key_from_name(name);
    } else {
        snprintf(why, whylen, "unknown setting '%s'", name);
        return why;
    }
    if (key < 0) { snprintf(why, whylen, "unknown key or button '%s'", name); return why; }
    int c = strcmp(val, "none") ? control_from_name(val) : -1;
    if (c < 0 && strcmp(val, "none")) { snprintf(why, whylen, "unknown pad control '%s'", val); return why; }
    if (!bindings_ready) defaults();
    bindings[key] = (int8_t)c;
    return NULL;
}

int input_map_parse(const char *text, InputMapReport report, void *ctx) {
    int bad = 0, lineno = 0;
    while (*text) {
        lineno++;
        size_t n = strcspn(text, "\n");
        char line[256], why[128];
        size_t len = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, text, len);
        line[len] = 0;
        text += n + (text[n] == '\n');
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
        char *start = line, *end = line + strlen(line);
        while (*start == ' ' || *start == '\t') start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
        *end = 0;
        if (!*start) continue;
        const char *err = n >= sizeof line ? "line too long" : parse_line(start, why, sizeof why);
        if (err) {
            bad++;
            if (report) report(ctx, lineno, err);
        }
    }
    return bad;
}

static void print_report(void *ctx, int line, const char *msg) { printf("[input] %s:%d: %s (line ignored)\n", (const char *)ctx, line, msg); }

int input_map_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char text[64 * 1024];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    int bad = input_map_parse(text, print_report, (void *)path);
    printf("[input] key map %s loaded (%d invalid line%s)\n", path, bad, bad == 1 ? "" : "s");
    return bad;
}

/* ---- mouse look ---- */
MouseCfg input_mouse_cfg = {1.0f, 0.15f, 0.05f, 0};
static MouseLook look;
static int acc_dx, acc_dy;
static double last_frame = -1;

static int16_t stick(float v, float deadzone) {
    float a = fabsf(v);
    if (a < 0.01f) return 0;
    a = deadzone + a * (1 - deadzone);
    if (a > 1) a = 1;
    return (int16_t)lrintf((v < 0 ? -a : a) * 32767);
}
static float clamp1(float v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

void mouse_look_update(MouseLook *m, const MouseCfg *c, double dx, double dy, double dt, int16_t *rx, int16_t *ry) {
    if (dt < 0.001) dt = 0.001;   /* pumps run every few ms; guard against a zero or huge step */
    if (dt > 0.1) dt = 0.1;
    float tx = (float)(dx / dt / MOUSE_FULL_SPEED) * c->sensitivity;
    float ty = (float)(-dy / dt / MOUSE_FULL_SPEED) * c->sensitivity * (c->invert_y ? -1 : 1);
    float alpha = c->smoothing > 0 ? 1 - expf((float)-dt / c->smoothing) : 1;
    m->x = clamp1(m->x + (tx - m->x) * alpha);
    m->y = clamp1(m->y + (ty - m->y) * alpha);
    *rx = stick(m->x, c->deadzone);
    *ry = stick(m->y, c->deadzone);
}

void input_mouse_motion(int dx, int dy) {
    acc_dx += dx;
    acc_dy += dy;
    if (dx || dy) input_note_device(INPUT_DEVICE_KBM);
}
void input_mouse_frame(double now) {
    double dt = last_frame < 0 ? 1.0 / 60 : now - last_frame;
    last_frame = now;
    int16_t rx, ry;
    mouse_look_update(&look, &input_mouse_cfg, acc_dx, acc_dy, dt, &rx, &ry);
    acc_dx = acc_dy = 0;
    input_set_mouse_stick(rx, ry);
}
void input_mouse_reset(void) {
    look = (MouseLook){0, 0};
    acc_dx = acc_dy = 0;
    last_frame = -1;
    input_set_mouse_stick(0, 0);
    for (int k = INPUT_MOUSE_LEFT; k <= INPUT_MOUSE_X2; k++) held[k] = 0;
    publish();
}

#ifdef INPUT_MAP_TEST
/* Game-independent test (build/input_map_test, run by tests/test_input_map.py). */
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static XPad pad(void) { XPad p; input_get(&p, NULL); return p; }

/* Feeds a raw X11-style key event queue the way the x11 backend does in its fallback mode and
 * returns, per event, whether W (left stick up) was seen held after it. */
static void feed_x11(const RawKeyEvent *ev, int n, int *held_after) {
    for (int i = 0; i < n; i++) {
        if (!ev[i].press && input_is_repeat_release(&ev[i], i + 1 < n ? &ev[i + 1] : NULL)) {
            held_after[i] = pad().ly == 32767;
            held_after[i + 1] = held_after[i];
            i++;   /* the synthetic release and its press are dropped together */
            continue;
        }
        input_key_event('w', ev[i].press, 0);
        held_after[i] = pad().ly == 32767;
    }
}

typedef struct { int n, line[16]; char msg[16][96]; } Reports;
static void collect(void *ctx, int line, const char *msg) {
    Reports *r = ctx;
    if (r->n < 16) { r->line[r->n] = line; snprintf(r->msg[r->n], sizeof r->msg[0], "%s", msg); r->n++; }
}

/* Runs `frames` updates of 1/60 s with the given motion per update; returns the last output. */
static void run(MouseLook *m, const MouseCfg *c, int frames, double dx, double dy, int16_t *rx, int16_t *ry) {
    for (int i = 0; i < frames; i++) mouse_look_update(m, c, dx, dy, 1.0 / 60, rx, ry);
}

/* Spin key: a fake clock, and the quadrant of the left stick as the player sees it (0 up,
 * 1 right, 2 down, 3 left; -1 centred). */
static double fake_now;
static double fake_clock(void) { return fake_now; }
static int quadrant(const XPad *p) {
    int x = p->lx, y = p->ly;
    if (!x && !y) return -1;
    if (abs(y) >= abs(x)) return y > 0 ? 0 : 2;
    return x > 0 ? 1 : 3;
}
/* Samples `polls` reads at 60 Hz from t0 while the spin is held; returns 1 when the stick stays
 * at full deflection, starts up, only ever moves on up, left, down, right, up (XInput +y is up)
 * and shows every quadrant for at least `min` polls. The title's car-theft screws take that
 * order as a clockwise turn (user play test, 2026-10-23: up, right, down, left turned them the wrong way). */
static int spins_title_clockwise(double t0, int polls, int min) {
    int count[4] = {0}, last = 0, ok = 1;
    for (int i = 0; i < polls; i++) {
        fake_now = t0 + i / 60.0;
        XPad p = pad();
        int q = quadrant(&p);
        double r = sqrt((double)p.lx * p.lx + (double)p.ly * p.ly);
        if (q < 0 || (i == 0 && q) || (q != last && q != (last + 3) % 4) || r < 32000 || r > 33500) ok = 0;
        if (q >= 0) { count[q]++; last = q; }
    }
    for (int q = 0; q < 4; q++) ok &= count[q] >= min;
    return ok;
}

int main(int argc, char **argv) {
    /* The shipped example config (argv[1]) parses cleanly and restates the defaults. */
    if (argc > 1) {
        input_map_defaults();
        int8_t def[INPUT_KEY_COUNT];
        MouseCfg dm = input_mouse_cfg;
        memcpy(def, bindings, sizeof def);
        memset(bindings, -1, sizeof bindings);
        input_spin_rate = 0;
        int bad = input_map_load_file(argv[1]);
        CHECK(bad == 0 && !memcmp(def, bindings, sizeof def) && !memcmp(&dm, &input_mouse_cfg, sizeof dm) &&
              input_spin_rate == INPUT_SPIN_RATE_DEFAULT,
              "%s: no invalid lines, same bindings, mouse settings and spin rate as the defaults", argv[1]);
        input_map_defaults();
    }
    /* T1: auto-repeat. A held W under X11 without detectable auto-repeat arrives as
     * press, (release, press) pairs with equal timestamps, then the real release. */
    const RawKeyEvent x[] = {{1, 25, 100}, {0, 25, 600}, {1, 25, 600}, {0, 25, 633}, {1, 25, 633},
                             {0, 25, 666}, {1, 25, 666}, {0, 25, 900}};
    int h[8];
    feed_x11(x, 8, h);
    CHECK(h[0] && h[1] && h[2] && h[3] && h[4] && h[5] && h[6] && !h[7], "X11 repeat pairs: W held continuously until the real release");
    const RawKeyEvent a = {0, 25, 600}, b = {1, 25, 601}, c = {1, 26, 600};
    CHECK(!input_is_repeat_release(&a, &b), "release then press with a later timestamp is a real release");
    CHECK(!input_is_repeat_release(&a, &c), "release then press of another key is a real release");
    CHECK(!input_is_repeat_release(&a, NULL), "release with nothing queued is a real release");
    CHECK(!input_is_repeat_release(&x[0], &x[0]), "a press is never a repeat release");

    /* SDL: repeat-flagged presses are ignored, also after the release. */
    input_key_event('w', 1, 0);
    int ok = pad().ly == 32767;
    for (int i = 0; i < 5; i++) { input_key_event('w', 1, 1); ok &= pad().ly == 32767; }
    input_key_event('w', 0, 0);
    ok &= pad().ly == 0;
    input_key_event('w', 1, 1);
    CHECK(ok && pad().ly == 0, "SDL repeat presses ignored; W held until released, a late repeat does not re-press");

    /* Two keys on one control. */
    input_key_event(INPUT_MOUSE_RIGHT, 1, 0);
    input_key_event(' ', 1, 0);
    input_key_event(INPUT_MOUSE_RIGHT, 0, 0);
    CHECK((pad().analog[0] == 0xFF) && input_key_down(' ') && !input_key_down(INPUT_MOUSE_RIGHT),
          "A stays held while Space is down after the right mouse button is released");
    input_key_event(INPUT_KEY_LEFT, 1, 0);
    input_release_all();
    XPad z = pad(), zero = {0};
    CHECK(!memcmp(&z, &zero, sizeof z), "release all clears the pad");
    CHECK(input_key_control(INPUT_KEY_LEFT) == IN_LEFT && input_key_control('m') == -1 && input_key_control(-5) == -1 &&
          input_key_control(INPUT_KEY_COUNT) == -1, "arrow binding, unbound and out-of-range keys");

    /* T2: mouse look. */
    MouseCfg mc = {1.0f, 0.0f, 0.05f, 0};
    MouseLook m = {0, 0};
    int16_t rx = 1, ry = 1;
    run(&m, &mc, 10, 0, 0, &rx, &ry);
    CHECK(rx == 0 && ry == 0, "no motion: centred stick");
    run(&m, &mc, 60, 10, 0, &rx, &ry);   /* 600 counts/s = 0.4 of full speed */
    CHECK(abs(rx - 13107) < 300 && ry == 0, "steady 600 counts/s right -> rx %d (expect ~13107)", rx);
    int16_t prev = rx, first;
    mouse_look_update(&m, &mc, 0, 0, 1.0 / 60, &first, &ry);
    int mono = first > 0 && first < prev, frames = 1;
    prev = first;
    while (prev && frames < 120) { mouse_look_update(&m, &mc, 0, 0, 1.0 / 60, &rx, &ry); mono &= rx <= prev; prev = rx; frames++; }
    CHECK(mono && !prev && frames <= 30, "stop: eases back to centre (first %d, centred after %d frames)", first, frames);
    m = (MouseLook){0, 0};
    run(&m, &mc, 60, 0, 10, &rx, &ry);
    CHECK(rx == 0 && ry < -13000, "mouse down -> stick down (ry %d)", ry);
    mc.invert_y = 1;
    m = (MouseLook){0, 0};
    run(&m, &mc, 60, 0, 10, &rx, &ry);
    CHECK(ry > 13000, "invert_y: mouse down -> stick up (ry %d)", ry);
    mc.invert_y = 0;
    mc.sensitivity = 2;
    m = (MouseLook){0, 0};
    run(&m, &mc, 60, -10, 0, &rx, &ry);
    CHECK(abs(rx + 26214) < 600, "sensitivity 2 doubles the deflection (rx %d)", rx);
    m = (MouseLook){0, 0};
    run(&m, &mc, 30, 5000, -5000, &rx, &ry);
    CHECK(rx == 32767 && ry == 32767, "fast motion clamps to full deflection (%d, %d)", rx, ry);
    mouse_look_update(&m, &mc, -5000, 0, 1.0 / 60, &rx, &ry);
    CHECK(rx < 0, "a clamped state reverses within one update (rx %d)", rx);
    mc = (MouseCfg){1.0f, 0.2f, 0.05f, 0};
    m = (MouseLook){0, 0};
    mouse_look_update(&m, &mc, 1, 0, 1.0 / 60, &rx, &ry);
    CHECK(rx >= 6553 && rx < 9000, "deadzone 0.2 lifts a one-count move past 20%% (rx %d)", rx);
    mc.smoothing = 0;
    m = (MouseLook){0, 0};
    mouse_look_update(&m, &mc, 0, 0, 0, &rx, &ry);
    CHECK(rx == 0 && ry == 0, "zero dt and no smoothing: no motion, no division by zero");

    /* Pointer feed and the right-stick merge in hle_input. */
    input_mouse_cfg = (MouseCfg){1.0f, 0.0f, 0.0f, 0};
    input_mouse_reset();
    input_mouse_frame(10.0);
    input_mouse_motion(4, 0);
    input_mouse_motion(6, 0);
    input_mouse_frame(10.0 + 1.0 / 60);
    XPad p = pad();
    CHECK(abs(p.rx - 13107) < 300, "motion accumulates per frame and drives the right stick (rx %d)", p.rx);
    input_key_event('j', 1, 0);
    CHECK(pad().rx == -32767, "right stick keys override the mouse");
    input_key_event('j', 0, 0);
    XPad ctl = {0};
    ctl.rx = 1000;
    input_set_controller(&ctl);
    CHECK(abs(pad().rx - 13107) < 300, "a moving mouse overrides the controller's right stick");
    input_mouse_frame(10.0 + 2.0 / 60);
    CHECK(pad().rx == 1000, "a still mouse leaves the controller's right stick");
    input_set_controller(NULL);
    input_key_event(INPUT_MOUSE_LEFT, 1, 0);
    input_key_event(INPUT_MOUSE_RIGHT, 1, 0);
    p = pad();
    CHECK(p.analog[IN_X - IN_A] == 0xFF && p.analog[IN_A - IN_A] == 0xFF, "left / right mouse buttons are X / A");
    input_mouse_motion(30, 0);
    input_mouse_frame(10.0 + 3.0 / 60);
    input_mouse_reset();
    p = pad();
    CHECK(p.rx == 0 && !p.analog[IN_X - IN_A] && !input_key_down(INPUT_MOUSE_RIGHT), "pointer release centres the stick and releases the buttons");
    input_mouse_cfg = (MouseCfg){1.0f, 0.15f, 0.05f, 0};

    /* T3: PC default layout. */
    input_map_defaults();
    CHECK(input_key_control('w') == IN_LS_UP && input_key_control('a') == IN_LS_LEFT && input_key_control(INPUT_MOUSE_LEFT) == IN_X &&
          input_key_control(INPUT_MOUSE_RIGHT) == IN_A && input_key_control(' ') == IN_A && input_key_control('f') == IN_B &&
          input_key_control('e') == IN_Y && input_key_control('r') == IN_X && input_key_control('q') == IN_LT &&
          input_key_control(INPUT_KEY_LSHIFT) == IN_RT && input_key_control(INPUT_KEY_LCTRL) == IN_BLACK &&
          input_key_control('\t') == IN_WHITE && input_key_control('c') == IN_LTHUMB && input_key_control(INPUT_MOUSE_MIDDLE) == IN_RTHUMB &&
          input_key_control('\r') == IN_START && input_key_control('\b') == IN_BACK && input_key_control(INPUT_KEY_ESCAPE) == -1 &&
          input_key_control(INPUT_KEY_F1) == -1, "PC default layout");
    CHECK(input_mouse_cfg.sensitivity == 1.0f && !input_mouse_cfg.invert_y, "default mouse settings");
    int names_ok = 1;
    const int keys[] = {'w', '7', ' ', '\r', '\b', '\t', '/', INPUT_KEY_UP, INPUT_KEY_RALT, INPUT_KEY_F1 + 1, INPUT_KEY_F1 + 11, INPUT_MOUSE_X2};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) names_ok &= input_key_name(keys[i]) && input_key_from_name(input_key_name(keys[i])) == keys[i];
    CHECK(names_ok && !strcmp(input_key_name(INPUT_KEY_LSHIFT) ? input_key_name(INPUT_KEY_LSHIFT) : "", "lshift") &&
          !strcmp(input_key_name(INPUT_MOUSE_LEFT) ? input_key_name(INPUT_MOUSE_LEFT) : "", "mouse.left") &&
          input_key_from_name("escape") == -1 && input_key_from_name("f1") == -1 && input_key_from_name("bogus") == -1,
          "key names round-trip; escape and f1 are not bindable");

    /* T3: config parsing. */
    static const char cfg[] =
        "# my layout\n"
        "key.W = left_stick_up\n"              /* 2: case-insensitive, already the default */
        "key.t = dpad_up   # trailing comment\n"
        "key.space = none\n"                   /* 4: unbind */
        "\n"
        "   mouse.right = b\n"                 /* 6 */
        "key.escape = a\n"                     /* 7: reserved */
        "key.nosuchkey = a\n"                  /* 8 */
        "key.g = jump\n"                       /* 9: unknown control */
        "mouse.sensitivity = 2.5\n"
        "mouse.invert_y = true\n"
        "mouse.sensitivity = fast\n"           /* 12: bad number, keeps 2.5 */
        "mouse.invert_y = maybe\n"             /* 13 */
        "this line has no equals\n"            /* 14 */
        "key.y = lsdown\n"                     /* 15: script names work too */
        "mouse.deadzone = 0.3\n"
        "mouse.smoothing = -1\n"               /* 17: out of range */
        "mouse.wheel = a\n"                    /* 18: unknown button */
        "key.u = rt";                           /* 19: valid, no final newline */
    Reports r = {0};
    int bad = input_map_parse(cfg, collect, &r);
    CHECK(bad == 8 && r.n == 8 && r.line[0] == 7 && r.line[1] == 8 && r.line[2] == 9 && r.line[3] == 12 && r.line[4] == 13 &&
          r.line[5] == 14 && r.line[6] == 17 && r.line[7] == 18,
          "invalid lines reported with their numbers (%d: %d %d %d %d %d %d %d %d)", bad, r.line[0], r.line[1], r.line[2],
          r.line[3], r.line[4], r.line[5], r.line[6], r.line[7]);
    CHECK(input_key_control('t') == IN_UP && input_key_control(' ') == -1 && input_key_control(INPUT_MOUSE_RIGHT) == IN_B &&
          input_key_control('y') == IN_LS_DOWN && input_key_control('w') == IN_LS_UP && input_key_control('g') == INPUT_CTL_SPIN &&
          input_key_control('u') == IN_RT,
          "valid binding lines applied, invalid ones ignored");
    CHECK(input_mouse_cfg.sensitivity == 2.5f && input_mouse_cfg.invert_y && input_mouse_cfg.deadzone == 0.3f &&
          input_mouse_cfg.smoothing == 0.05f, "mouse settings applied, bad values keep the previous ones");
    printf("%s\n", r.n ? r.msg[0] : "");
    CHECK(input_map_load_file("/nonexistent/warriors-input.cfg") < 0, "missing config file -> -1");
    bindings_ready = 0;   /* as at start-up: bindings not yet initialised when the file is read */
    input_map_parse("mouse.sensitivity = 3\nkey.t = a\n", collect, &r);
    CHECK(input_mouse_cfg.sensitivity == 3.0f && input_key_control('t') == IN_A && input_key_control('w') == IN_LS_UP,
          "a mouse setting before the first binding survives the lazy default bindings");
    /* T4: spin key. G held turns the left stick (up, left, down, right) at spin.rate turns/s, over WASD and
     * the controller; released, the stick follows WASD again. */
    input_map_defaults();
    double (*real_clock)(void) = input_clock;
    input_clock = fake_clock;
    CHECK(input_key_control('g') == INPUT_CTL_SPIN && input_spin_rate == INPUT_SPIN_RATE_DEFAULT,
          "G is the spin key by default, %.1f turns/s", input_spin_rate);
    input_key_event('a', 1, 0);
    CHECK(pad().lx == -32767 && pad().ly == 0, "WASD alone: A is left stick left");
    input_key_event('a', 0, 0);
    input_key_event('d', 1, 0);
    fake_now = 100.0;
    input_key_event('g', 1, 0);
    CHECK(spins_title_clockwise(100.0, 30, 6), "G held: up, left, down, right within one turn (30 polls at 60 Hz, 2 turns/s)");
    fake_now = 100.5;
    CHECK(pad().ly > 32000 && abs(pad().lx) < 300, "after one turn (0.5 s) the stick is up again");
    fake_now = 100.0 + 1.0 / 4;
    p = pad();
    CHECK(p.ly < -32000 && abs(p.lx) < 300, "half a turn in (0.25 s) is down (%d, %d)", p.lx, p.ly);
    fake_now = 100.0 + 0.125 / 2;
    p = pad();
    CHECK(p.lx < -23000 && p.ly > 23000, "an eighth of a turn in is up-left (%d, %d)", p.lx, p.ly);
    XPad pc = {0};
    pc.lx = -1000;
    input_set_controller(&pc);
    fake_now = 100.0 + 3.0 / 8;
    CHECK(pad().lx > 32000, "the spin overrides the controller's left stick (three quarters in: right)");
    input_set_controller(NULL);
    input_key_event('g', 0, 0);
    CHECK(pad().lx == 32767 && pad().ly == 0, "G released: the left stick follows D again");
    input_key_event('d', 0, 0);
    CHECK(pad().lx == 0 && pad().ly == 0, "all released: centred");
    fake_now = 200.0;
    input_key_event('g', 1, 0);
    CHECK(pad().ly > 32000 && abs(pad().lx) < 300, "each press starts at up");
    input_key_event('g', 0, 0);
    r = (Reports){0};
    bad = input_map_parse("key.h = spin_left_stick\nspin.rate = 4\nspin.rate = 0\nspin.rate = fast\n", collect, &r);
    CHECK(bad == 2 && r.line[0] == 3 && r.line[1] == 4 && input_key_control('h') == INPUT_CTL_SPIN && input_spin_rate == 4.0f,
          "config: key.h = spin_left_stick and spin.rate = 4 apply, out-of-range rates are reported");
    input_key_event('h', 1, 0);
    CHECK(spins_title_clockwise(200.0, 15, 3), "H at 4 turns/s: one turn in 15 polls");
    input_key_event('h', 0, 0);
    input_map_defaults();
    CHECK(input_spin_rate == INPUT_SPIN_RATE_DEFAULT && input_key_control('h') == -1, "defaults restore the spin rate and key");
    input_clock = real_clock;
    input_map_defaults();

    /* Button icons: the key a prompt shows, and the last used device. */
    CHECK(input_key_for_control(IN_X) == INPUT_MOUSE_LEFT && input_key_for_control(IN_A) == INPUT_MOUSE_RIGHT &&
          input_key_for_control(IN_RTHUMB) == INPUT_MOUSE_MIDDLE && input_key_for_control(IN_B) == 'f' &&
          input_key_for_control(IN_START) == '\r' && input_key_for_control(IN_UP) == INPUT_KEY_UP && input_key_for_control(-1) == -1,
          "shown key: the mouse button first, else the bound key");
    input_map_parse("mouse.left = none\nkey.z = x\nmouse.right = none\nkey.e = none\n", collect, &r);
    CHECK(input_key_for_control(IN_X) == 'r' && input_key_for_control(IN_A) == ' ' && input_key_for_control(IN_Y) == -1,
          "shown key after a remap: lowest key code (R before Z), nothing bound -> -1");
    input_map_defaults();
    XPad idle = {0}, moved = {0};
    moved.ly = 30000;
    input_note_device(INPUT_DEVICE_NONE);
    input_set_controller(&idle);
    CHECK(input_last_device() == INPUT_DEVICE_NONE, "an idle controller is not controller use");
    input_set_controller(&moved);
    CHECK(input_last_device() == INPUT_DEVICE_PAD, "a stick past half way: last device = controller");
    input_key_event('w', 1, 0);
    input_key_event('w', 0, 0);
    CHECK(input_last_device() == INPUT_DEVICE_KBM, "a key press: last device = keyboard/mouse");
    input_set_controller(&moved);
    input_mouse_motion(3, 0);
    CHECK(input_last_device() == INPUT_DEVICE_KBM, "mouse motion: last device = keyboard/mouse");
    input_set_controller(NULL);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
