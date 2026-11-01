/* Game-independent test of the SDL controller -> port assignment in src/video.c (included here
 * for its static state): opens the sdl backend on SDL's dummy video driver and plugs SDL virtual
 * game controllers (SDL 2.0.24+ for distinct vendor/product ids). Prints SKIP and exits 0 when
 * SDL2, the dummy driver or virtual joysticks are not available, or a real controller is attached. */
#include "../src/video.c"
#include <stdlib.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* SDL_VirtualJoystickDesc, version 1 (SDL 2.24+). */
typedef struct {
    uint16_t version, type, naxes, nbuttons, nhats, vendor_id, product_id, padding;
    uint32_t button_mask, axis_mask;
    const char *name;
    void *userdata, *update, *set_player_index, *rumble, *rumble_triggers, *set_led, *send_effect;
} VirtualDesc;
static int (*attach_ex)(const VirtualDesc *desc);
static int (*detach)(int device_index);
static int (*set_button)(void *joystick, int button, uint8_t value);
static int (*is_virtual)(int device_index);

static void pump(void) { for (int i = 0; i < 3; i++) video_pump(); }
static int count_ports(void) {
    int n = 0;
    for (int p = 0; p < INPUT_PORTS; p++) n += sdl.port[p].pad != NULL;
    return n;
}
static const char *const names[] = {"pad A", "pad B", "pad C", "pad D", "pad E", "pad raw"};
static void plug(int id) {   /* virtual game controller with its own product id (and GUID) */
    VirtualDesc d = {1, 1 /* SDL_JOYSTICK_TYPE_GAMECONTROLLER */, 6, 15, 0, 0x1234, (uint16_t)(0x100 + id), 0,
                     0, 0, names[id], NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    if (attach_ex(&d) < 0) printf("attach failed: %s\n", sdl.GetError());
    pump();
}
static int port_named(int id) {
    for (int p = 0; p < INPUT_PORTS; p++) if (sdl.port[p].pad && !strcmp(sdl.port[p].name, names[id])) return p;
    return -1;
}
static void unplug(int id) {
    for (int i = 0; i < sdl.NumJoysticks(); i++) {
        if (!is_virtual(i)) continue;
        void *pad = sdl.GameControllerOpen(i);
        int match = pad && !strcmp(sdl.GameControllerName(pad), names[id]);
        if (pad) sdl.GameControllerClose(pad);
        if (match) { detach(i); break; }
    }
    pump();
}

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    const char *b = video_open("sdl", 640, 480, "video_pads_test");
    if (strcmp(b, "sdl") || !sdl.GameControllerOpen) { printf("SKIP: no SDL2 window with game controller support (%s)\n", b); return 0; }
    *(void **)&attach_ex = plat_dlsym(sdl.lib, "SDL_JoystickAttachVirtualEx");
    *(void **)&detach = plat_dlsym(sdl.lib, "SDL_JoystickDetachVirtual");
    *(void **)&set_button = plat_dlsym(sdl.lib, "SDL_JoystickSetVirtualButton");
    *(void **)&is_virtual = plat_dlsym(sdl.lib, "SDL_JoystickIsVirtual");
    if (!attach_ex || !detach || !set_button || !is_virtual || !sdl.GameControllerGetJoystick || !sdl.JoystickGetGUID) {
        printf("SKIP: SDL2 without virtual joysticks\n");
        video_close();
        return 0;
    }
    if (count_ports()) { printf("SKIP: real controller(s) attached\n"); video_close(); return 0; }

    CHECK(input_connected_ports() == 1, "no controller: port 0 only (%x)", input_connected_ports());
    plug(0);
    CHECK(port_named(0) == 0 && count_ports() == 1 && input_connected_ports() == 1, "first controller -> port 0 (ports %x)", input_connected_ports());
    plug(1);
    CHECK(port_named(1) == 1 && count_ports() == 2 && input_connected_ports() == 3, "second controller -> port 1 (ports %x)", input_connected_ports());
    pump();
    CHECK(count_ports() == 2, "rescans keep the assignment (no second open)");

    /* Start on the second controller reaches port 1 only. */
    set_button(sdl.GameControllerGetJoystick(sdl.port[1].pad), 6 /* SDL_CONTROLLER_BUTTON_START */, 1);
    pump();
    XPad p0, p1;
    input_get_port(0, &p0, NULL);
    input_get_port(1, &p1, NULL);
    CHECK((p1.buttons & 0x10) && !(p0.buttons & 0x10), "Start on controller 2 -> port 1 only (p0 %x p1 %x)", p0.buttons, p1.buttons);
    set_button(sdl.GameControllerGetJoystick(sdl.port[1].pad), 6, 0);
    pump();

    plug(2);
    plug(3);
    CHECK(count_ports() == 4 && input_connected_ports() == 0xF, "four controllers -> ports 0-3");
    plug(4);
    CHECK(count_ports() == 4 && port_named(4) < 0, "a fifth controller gets no port");
    unplug(1);
    CHECK(port_named(4) == 1 && input_connected_ports() == 0xF, "the waiting fifth controller takes freed port 1");
    unplug(4);
    unplug(0);
    CHECK(count_ports() == 2 && input_connected_ports() == 0xD, "ports 0 and 1 freed, port 0 still connected (ports %x)", input_connected_ports());
    plug(4);
    CHECK(port_named(4) == 1, "a returning controller takes its previous port (1) when free, not the lowest (%d)", port_named(4));
    plug(1);
    CHECK(port_named(1) == 0, "a controller whose previous port is taken gets the lowest free one (%d)", port_named(1));

    for (int id = 0; id < 5; id++) unplug(id);
    CHECK(count_ports() == 0 && input_connected_ports() == 1, "all removed: port 0 only (ports %x)", input_connected_ports());

    /* gamecontrollerdb.txt: a joystick SDL does not know becomes a game controller with the
     * player's mapping line for this platform; the other platform's line is skipped. */
    struct SdlGuid (*device_guid)(int) = NULL;
    void (*guid_string)(struct SdlGuid, char *, int) = NULL;
    const char *(*name_for_index)(int) = NULL;
    *(void **)&device_guid = plat_dlsym(sdl.lib, "SDL_JoystickGetDeviceGUID");
    *(void **)&guid_string = plat_dlsym(sdl.lib, "SDL_JoystickGetGUIDString");
    *(void **)&name_for_index = plat_dlsym(sdl.lib, "SDL_JoystickNameForIndex");
    CHECK(sdl_mappings_from("/nonexistent/gamecontrollerdb.txt") == -1, "a missing mapping file -> -1");
    VirtualDesc raw = {1, 0 /* SDL_JOYSTICK_TYPE_UNKNOWN */, 6, 12, 1, 0x4321, 0x77, 0,
                       0, 0, names[5], NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    int dev = -1;
    if (device_guid && guid_string && name_for_index && attach_ex(&raw) >= 0) {
        pump();
        for (int i = 0; i < sdl.NumJoysticks(); i++)
            if (is_virtual(i) && name_for_index(i) && !strcmp(name_for_index(i), names[5])) dev = i;
    }
    if (dev < 0) printf("SKIP: no unknown-type virtual joystick\n");
    else {
        CHECK(!sdl.IsGameController(dev) && count_ports() == 0, "an unknown joystick is no game controller");
        char guid[33], path[4200];
        guid_string(device_guid(dev), guid, sizeof guid);
        snprintf(path, sizeof path, "%svideo_pads_test-gamecontrollerdb.txt", plat_program_dir());
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "# test mappings\n%s,pad raw,a:b3,b:b2,lefttrigger:a2,righttrigger:+a5,leftx:a0,lefty:a1~,dpup:h0.1,platform:Mac OS X,\n", guid);
            fprintf(f, "%s,pad raw,a:b3,b:b2,lefttrigger:a2,righttrigger:+a5,leftx:a0,lefty:a1~,dpup:h0.1,platform:%s,\n", guid,
#ifdef _WIN32
                    "Windows");
#else
                    "Linux");
#endif
            fclose(f);
        }
        int n = sdl_mappings_from(path);
        CHECK(n == 1, "one mapping for this platform added from the file (%d)", n);
        CHECK(sdl.IsGameController(dev), "the joystick is a game controller with it");
        sdl_pad_open();   /* at start-up the mappings are read before the controllers are opened */
        pump();
        CHECK(port_named(5) == 0, "and takes port 0 (%d)", port_named(5));
        if (port_named(5) == 0) {
            set_button(sdl.GameControllerGetJoystick(sdl.port[0].pad), 3, 1);   /* raw button 3 = A */
            pump();
            input_get_port(0, &p0, NULL);
            CHECK(p0.analog[0] > 0, "raw button 3 reads as A through the mapping (%d)", p0.analog[0]);
            set_button(sdl.GameControllerGetJoystick(sdl.port[0].pad), 3, 0);
        }
        unplug(5);
        remove(path);
    }
    video_close();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
