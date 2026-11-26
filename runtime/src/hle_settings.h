/* The in-game settings menu in the running game: the glue
 * between the pure menu (src/settings_menu.h), the launcher sync (src/settings_sync.h), the live
 * apply queue (src/settings_apply.h) and the runtime.
 *
 * Opening: F10 (input_key_hook), or Back + Start held together 0.5 s on the pad of port 0. B, Back,
 * Escape or F10 again close it. While it is open the game gets a neutral pad; it pauses through its
 * own pause screen: Back + Start already pressed Start for the game, F10 presses Start once for it
 * when the game clock is running (not when it is already stopped: a pause screen, a load).
 * Controls: up / down move, left / right change the value, A / Enter toggle or confirm, LB / RB
 * (white / black) or Q / E switch tabs.
 *
 * Changes are recorded with settings_sync_set (the launcher saves them) and applied live at the next
 * host frame boundary when settings_apply can (volumes, rumble, audio air / occlusion, TAA
 * sharpness, parallax, neon glow strength, the temporal upscaling mode while it runs, dynamic
 * resolution); the rest waits for "Reiniciar ahora" (settings_sync_restart: exit 86, the launcher
 * relaunches with the new profile). Log lines: [settings]. */
#ifndef HLE_SETTINGS_H
#define HLE_SETTINGS_H
#include <stdint.h>

/* main.c, after settings_sync_init and before the mods: the disc folder (the game's font) and the
 * game's --language (the menu's language when the launcher did not pass its own). Installs the
 * input filter (chained before next_filter, the mods' menu), the escape and key hooks, the menu
 * view and the frame hook. */
void hle_settings_install(const char *disc_dir, const char *game_language);
/* Hooks chained after the settings menu (a mod's menu): set by main.c after hle_settings_install. */
extern int (*hle_settings_next_filter)(uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly);
extern int (*hle_settings_next_escape)(void);
extern void (*hle_settings_next_key)(int key, int down);   /* keys the settings menu does not take */

#endif
