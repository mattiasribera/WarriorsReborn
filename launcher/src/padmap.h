/* The Controls page's controller mapper, without SDL: the Xbox inputs it asks for, the capture of
 * one input from raw joystick snapshots (SDL_Joystick buttons, axes and hats, not the
 * GameController layer, so a wrong mapping does not get in the way), the SDL GameController
 * mapping line built from the captured inputs and the gamecontrollerdb.txt merge (one line per
 * controller GUID and platform). main.cpp feeds the snapshots and applies the line. */
#ifndef LAUNCHER_PADMAP_H
#define LAUNCHER_PADMAP_H
#include <string>
#include <vector>

namespace launcher {

/* What a step accepts: a button (also a hat direction or half an axis, for D-pads and triggers
 * that report that way), a trigger (an axis preferred over a button it also reports), or one
 * direction of a stick (a whole axis). */
enum class PadTargetKind { Button, Trigger, StickX, StickY };

/* The wizard's steps, in order. part: the SDL_GameControllerButton it draws on the preview, or
 * 100 + the SDL_GameControllerAxis. */
struct PadTarget {
    const char *field;          /* the SDL mapping field: a, b, lefttrigger, leftx, ... */
    PadTargetKind kind;
    int part;
    bool optional;              /* Guide: many pads have none */
};
extern const PadTarget PAD_TARGETS[];
constexpr int PAD_TARGET_COUNT = 21;

/* One raw joystick snapshot (SDL_JoystickGetButton / GetAxis / GetHat). */
struct PadRaw {
    std::vector<bool> buttons;
    std::vector<int> axes;      /* -32768..32767 */
    std::vector<int> hats;      /* SDL_HAT_* bits: 1 up, 2 right, 4 down, 8 left */
};

/* One raw input and how it maps: b3, h0.4, +a2 / -a2 (half an axis from its centre), a2 (the
 * whole axis; for a trigger resting at -32768), a2~ (inverted). */
struct PadBinding {
    enum Kind { None, Button, Axis, Hat } kind = None;
    int index = 0;
    int hat_mask = 0;
    int half = 0;               /* axis: +1 / -1 half axis, 0 the whole axis */
    bool invert = false;        /* whole axis: ~ */
    std::string str() const;    /* the SDL mapping value, "" for None */
};
/* The same physical input (the same button or hat direction, or overlapping parts of an axis). */
bool pad_same_input(const PadBinding &a, const PadBinding &b);

/* Thresholds on the distance from an axis's rest value: an input counts from DETECT, and is let
 * go below RELEASE; jitter under RELEASE is noise. */
constexpr int PAD_AXIS_DETECT = 16000;
constexpr int PAD_AXIS_RELEASE = 8000;

/* Captures one input per step from successive snapshots. Buttons count at rest as released and
 * hats as centred; the axes' rest values are the snapshot given to start (and are corrected when an
 * axis, once let go, settles at the other end, as triggers that read 0 until first touched do).
 * After start and after each captured input everything must be let go before the next one counts. */
class PadCapture {
public:
    void start(const PadRaw &rest);
    void restart();             /* the same rest values; wait for everything to be let go */
    /* One frame: true when an input for this kind of step was captured (into out). */
    bool feed(const PadRaw &now, PadTargetKind kind, PadBinding &out);
    bool waiting_release() const { return state_ == WaitRelease; }

private:
    enum State { WaitRelease, Armed, AxisHeld, ButtonHeld } state_ = WaitRelease;
    std::vector<int> rest_, stable_, last_;
    int axis_ = 0, peak_ = 0, button_ = 0, frames_ = 0;
    void sync_axes(const PadRaw &now);
    bool released(const PadRaw &now);
    PadBinding axis_binding(PadTargetKind kind) const;
};

/* "GUID,Name,a:b0,...,platform:Linux," with the fields in alphabetical order (as in the community
 * gamecontrollerdb.txt); targets left None are not written. Commas in the name become spaces.
 * binds holds PAD_TARGET_COUNT entries in PAD_TARGETS order. */
std::string pad_mapping_line(const std::string &guid, const std::string &name, const PadBinding *binds,
                             const std::string &platform);

/* The SDL platform name of this build ("Windows" or "Linux"), as SDL_GetPlatform reports it. */
const char *pad_platform();

/* gamecontrollerdb.txt text with the line for guid on platform replaced by line (appended when
 * there was none; removed when line is empty). Lines without a platform field match every
 * platform. Comments, blank lines and the other controllers' lines are kept as they are. */
std::string pad_db_merge(const std::string &db, const std::string &guid, const std::string &platform,
                         const std::string &line);
/* The line for guid on platform, "" when there is none. */
std::string pad_db_find(const std::string &db, const std::string &guid, const std::string &platform);

} // namespace launcher
#endif
