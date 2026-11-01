#include "i18n.h"
#include "platform.h"
#include <cstdarg>
#include <cstdio>

namespace launcher {

static const char *const STRINGS[(int)S::Count][(int)Lang::Count] = {
#define X(id, en, es) {en, es},
#include "i18n_strings.h"
#undef X
};

static Lang current = Lang::En;

void set_ui_lang(Lang lang) { current = lang; }
Lang ui_lang() { return current; }
const char *tr(S id) { return STRINGS[(int)id][(int)current]; }
const char *tr(S id, Lang lang) { return STRINGS[(int)id][(int)lang]; }

std::string trf(S id, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, id);
    vsnprintf(buf, sizeof buf, tr(id), ap);
    va_end(ap);
    return buf;
}

const char *lang_key(Lang lang) { return lang == Lang::Es ? "es" : "en"; }

Lang lang_from_setting(const std::string &key) {
    if (key == "es") return Lang::Es;
    if (key == "en") return Lang::En;
    return system_lang();
}

Lang system_lang() { return os_language() == "es" ? Lang::Es : Lang::En; }

} // namespace launcher
