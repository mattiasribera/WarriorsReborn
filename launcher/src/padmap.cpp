#include "padmap.h"
#include <cctype>
#include <cstdlib>

namespace launcher {

/* part: SDL_GameControllerButton (A B X Y Back Guide Start LStick RStick LB RB Up Down Left
 * Right), or 100 + SDL_GameControllerAxis (LX LY RX RY LT RT). */
const PadTarget PAD_TARGETS[PAD_TARGET_COUNT] = {
    {"a", PadTargetKind::Button, 0, false},
    {"b", PadTargetKind::Button, 1, false},
    {"x", PadTargetKind::Button, 2, false},
    {"y", PadTargetKind::Button, 3, false},
    {"leftshoulder", PadTargetKind::Button, 9, false},
    {"rightshoulder", PadTargetKind::Button, 10, false},
    {"lefttrigger", PadTargetKind::Trigger, 104, false},
    {"righttrigger", PadTargetKind::Trigger, 105, false},
    {"back", PadTargetKind::Button, 4, false},
    {"start", PadTargetKind::Button, 6, false},
    {"guide", PadTargetKind::Button, 5, true},
    {"leftstick", PadTargetKind::Button, 7, false},
    {"rightstick", PadTargetKind::Button, 8, false},
    {"dpup", PadTargetKind::Button, 11, false},
    {"dpdown", PadTargetKind::Button, 12, false},
    {"dpleft", PadTargetKind::Button, 13, false},
    {"dpright", PadTargetKind::Button, 14, false},
    {"leftx", PadTargetKind::StickX, 100, false},
    {"lefty", PadTargetKind::StickY, 101, false},
    {"rightx", PadTargetKind::StickX, 102, false},
    {"righty", PadTargetKind::StickY, 103, false},
};

std::string PadBinding::str() const {
    switch (kind) {
    case Button: return "b" + std::to_string(index);
    case Hat: return "h" + std::to_string(index) + "." + std::to_string(hat_mask);
    case Axis: return std::string(half > 0 ? "+" : half < 0 ? "-" : "") + "a" + std::to_string(index) + (half == 0 && invert ? "~" : "");
    default: return "";
    }
}

bool pad_same_input(const PadBinding &a, const PadBinding &b) {
    if (a.kind != b.kind || a.kind == PadBinding::None || a.index != b.index) return false;
    if (a.kind == PadBinding::Hat) return a.hat_mask == b.hat_mask;
    if (a.kind == PadBinding::Axis) return a.half == 0 || b.half == 0 || a.half == b.half;
    return true;
}

/* ---- capture ---- */

/* An axis left at another value than its rest for this long (60 frames a second) is at rest
 * there: a trigger that read 0 until first touched. */
static const int SETTLE_FRAMES = 90;
/* A button seen on a trigger step waits this long for an axis the trigger also reports. */
static const int TRIGGER_BUTTON_FRAMES = 12;

void PadCapture::start(const PadRaw &rest) {
    rest_ = rest.axes;
    stable_.assign(rest_.size(), 0);
    last_ = rest_;
    state_ = WaitRelease;
}

void PadCapture::restart() { state_ = WaitRelease; }

/* A device can report more axes than at start (it was opened later): those rest where they are. */
void PadCapture::sync_axes(const PadRaw &now) {
    for (size_t i = rest_.size(); i < now.axes.size(); i++) {
        rest_.push_back(now.axes[i]);
        stable_.push_back(0);
        last_.push_back(now.axes[i]);
    }
}

bool PadCapture::released(const PadRaw &now) {
    bool all = true;
    for (bool b : now.buttons) all &= !b;
    for (int h : now.hats) all &= h == 0;
    for (size_t i = 0; i < now.axes.size(); i++) {
        int v = now.axes[i];
        stable_[i] = std::abs(v - last_[i]) < 1000 ? stable_[i] + 1 : 0;
        last_[i] = v;
        if (std::abs(v - rest_[i]) < PAD_AXIS_RELEASE) continue;
        if (stable_[i] >= SETTLE_FRAMES) rest_[i] = v;
        else all = false;
    }
    return all;
}

PadBinding PadCapture::axis_binding(PadTargetKind kind) const {
    PadBinding b;
    b.kind = PadBinding::Axis;
    b.index = axis_;
    int r = rest_[axis_], dir = peak_ > r ? 1 : -1;
    if (kind == PadTargetKind::StickX || kind == PadTargetKind::StickY) {
        b.invert = dir != (kind == PadTargetKind::StickX ? 1 : -1);   /* SDL: right and down are positive */
    } else if (std::abs(r) <= 16384) {
        b.half = dir;          /* rests at its centre: the half it moves into */
    } else {
        b.invert = r > 0;      /* rests at one end: the whole axis, from that end */
    }
    return b;
}

bool PadCapture::feed(const PadRaw &now, PadTargetKind kind, PadBinding &out) {
    sync_axes(now);
    bool stick = kind == PadTargetKind::StickX || kind == PadTargetKind::StickY;
    /* the axis moved furthest from its rest, from the detection threshold */
    int moved = -1, dist = PAD_AXIS_DETECT - 1;
    for (size_t i = 0; i < now.axes.size(); i++)
        if (std::abs(now.axes[i] - rest_[i]) > dist) moved = (int)i, dist = std::abs(now.axes[i] - rest_[i]);
    switch (state_) {
    case WaitRelease:
        if (released(now)) state_ = Armed;
        return false;
    case AxisHeld: {
        if (axis_ >= (int)now.axes.size()) { state_ = WaitRelease; return false; }   /* another device */
        int v = now.axes[axis_], r = rest_[axis_];
        if ((v > r) == (peak_ > r) && std::abs(v - r) > std::abs(peak_ - r)) peak_ = v;
        bool back = std::abs(v - r) < PAD_AXIS_RELEASE;
        /* let go past its rest, to the other end: that end is its rest */
        bool other_end = (v > r) != (peak_ > r) && std::abs(v - r) > 16384;
        if (!back && !other_end) return false;
        if (other_end) rest_[axis_] = v < r ? -32768 : 32767;
        out = axis_binding(kind);
        state_ = WaitRelease;
        return true;
    }
    case ButtonHeld:
        if (moved >= 0) {   /* the trigger's axis */
            axis_ = moved, peak_ = now.axes[moved], state_ = AxisHeld;
            return false;
        }
        if (++frames_ < TRIGGER_BUTTON_FRAMES && button_ < (int)now.buttons.size() && now.buttons[button_]) return false;
        out = PadBinding();
        out.kind = PadBinding::Button, out.index = button_;
        state_ = WaitRelease;
        return true;
    case Armed:
        break;
    }
    if (!stick) {
        for (size_t i = 0; i < now.buttons.size(); i++) {
            if (!now.buttons[i]) continue;
            if (kind == PadTargetKind::Trigger && moved < 0) {
                button_ = (int)i, frames_ = 0, state_ = ButtonHeld;
                return false;
            }
            if (kind == PadTargetKind::Trigger) break;   /* its axis moved too: taken below */
            out = PadBinding();
            out.kind = PadBinding::Button, out.index = (int)i;
            state_ = WaitRelease;
            return true;
        }
        for (size_t h = 0; h < now.hats.size(); h++) {
            int v = now.hats[h];
            if (v != 1 && v != 2 && v != 4 && v != 8) continue;   /* centred or a diagonal */
            out = PadBinding();
            out.kind = PadBinding::Hat, out.index = (int)h, out.hat_mask = v;
            state_ = WaitRelease;
            return true;
        }
    }
    if (moved >= 0) axis_ = moved, peak_ = now.axes[moved], state_ = AxisHeld;
    return false;
}

/* ---- mapping line and gamecontrollerdb.txt ---- */

std::string pad_mapping_line(const std::string &guid, const std::string &name, const PadBinding *binds,
                             const std::string &platform) {
    std::string n;
    for (char ch : name) n += ch == ',' || ch == '\r' || ch == '\n' ? ' ' : ch;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(0, 1);
    if (n.empty()) n = "Controller";
    std::vector<std::pair<std::string, std::string>> fields;
    for (int i = 0; i < PAD_TARGET_COUNT; i++)
        if (binds[i].kind != PadBinding::None) fields.emplace_back(PAD_TARGETS[i].field, binds[i].str());
    for (size_t i = 1; i < fields.size(); i++)   /* alphabetical, as in the community file */
        for (size_t j = i; j > 0 && fields[j].first < fields[j - 1].first; j--) std::swap(fields[j], fields[j - 1]);
    std::string line = guid + "," + n + ",";
    for (const auto &f : fields) line += f.first + ":" + f.second + ",";
    return line + "platform:" + platform + ",";
}

const char *pad_platform() {
#ifdef _WIN32
    return "Windows";
#else
    return "Linux";
#endif
}

static std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/* Whether a db line (without its line end) is the mapping of guid on platform. */
static bool line_matches(const std::string &line, const std::string &guid, const std::string &platform) {
    size_t comma = line.find(',');
    if (line.empty() || line[0] == '#' || comma == std::string::npos) return false;
    if (lower(line.substr(0, comma)) != lower(guid)) return false;
    size_t at = line.find(",platform:");
    if (at == std::string::npos) return true;
    size_t start = at + 10, end = line.find(',', start);
    return lower(line.substr(start, end == std::string::npos ? std::string::npos : end - start)) == lower(platform);
}

/* The db's lines, each with its line end ("\n", "\r\n", or none for a last line without one). */
static std::vector<std::pair<std::string, std::string>> db_lines(const std::string &db) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos < db.size()) {
        size_t nl = db.find('\n', pos);
        std::string text = db.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos), end;
        if (nl != std::string::npos) end = "\n";
        if (!text.empty() && text.back() == '\r') text.pop_back(), end = "\r" + end;
        out.emplace_back(text, end);
        pos = nl == std::string::npos ? db.size() : nl + 1;
    }
    return out;
}

std::string pad_db_merge(const std::string &db, const std::string &guid, const std::string &platform,
                         const std::string &line) {
    std::string out;
    bool placed = line.empty();
    for (const auto &l : db_lines(db)) {
        if (!line_matches(l.first, guid, platform)) {
            out += l.first + (l.second.empty() ? "\n" : l.second);
            continue;
        }
        if (!placed) out += line + "\n";   /* the first one replaced, later duplicates dropped */
        placed = true;
    }
    if (!placed) out += line + "\n";
    return out;
}

std::string pad_db_find(const std::string &db, const std::string &guid, const std::string &platform) {
    for (const auto &l : db_lines(db))
        if (line_matches(l.first, guid, platform)) return l.first;
    return "";
}

} // namespace launcher
