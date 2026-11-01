/* The launcher's own UI language (English or Spanish), independent of the game's language
 * (--language). Every visible string has an id in i18n_strings.h with both texts; tr() gives the
 * current language's text and trf() formats one with %s / %d placeholders. The language comes
 * from the [launcher] ui_language setting, else from the system (Spanish for es-*, else
 * English). */
#ifndef LAUNCHER_I18N_H
#define LAUNCHER_I18N_H
#include <string>

namespace launcher {

enum class S {
#define X(id, en, es) id,
#include "i18n_strings.h"
#undef X
    Count
};

enum class Lang { En, Es, Count };

void set_ui_lang(Lang lang);
Lang ui_lang();
const char *tr(S id);
const char *tr(S id, Lang lang);
std::string trf(S id, ...); /* tr(id) as a printf format */

const char *lang_key(Lang lang);                    /* en|es */
/* The language of a ui_language setting: "en" or "es", else (empty or unknown) the system's. */
Lang lang_from_setting(const std::string &key);
Lang system_lang();

} // namespace launcher
#endif
