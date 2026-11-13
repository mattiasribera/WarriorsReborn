/* The launcher's UI strings: X(id, English, Spanish), see i18n.h. Placeholders (%s, %d) must be
 * the same, in the same order, in both languages (launcher_test checks it). */
X(page_home, "Home", "Inicio")
X(page_display, "Display", "Pantalla")
X(page_game, "Game", "Juego")
X(page_controls, "Controls", "Controles")
X(page_options, "Options", "Opciones")
X(launcher_caps, "LAUNCHER", "LANZADOR")
X(profile_caps, "PROFILE", "PERFIL")
X(play, "PLAY", "JUGAR")
X(running_caps, "RUNNING", "EN CURSO")
X(setup_step, "Setup: step %d of 4", "Configuración: paso %d de 4")
X(game_running, "The game is running", "El juego está en marcha")
X(ready, "Ready to play", "Listo para jugar")
X(could_not_start, "Could not start the game: %s", "No se pudo iniciar el juego: %s")
X(err_running, "the game is already running", "el juego ya está en marcha")
X(err_cannot_create, "cannot create %s", "no se puede crear %s")
X(cannot_open, "Cannot open %s", "No se puede abrir %s")
X(xbe_found, "Found: The Warriors (Xbox)", "Encontrado: The Warriors (Xbox)")
X(xbe_missing,
  "No default.xbe in this folder. Choose the folder that contains default.xbe.",
  "Esta carpeta no tiene default.xbe. Elige la carpeta que contiene default.xbe.")
X(xbe_unreadable, "The default.xbe in this folder cannot be read.", "No se puede leer el default.xbe de esta carpeta.")
X(xbe_not_xbe,
  "The default.xbe in this folder is not an Xbox game file.",
  "El default.xbe de esta carpeta no es un archivo de juego de Xbox.")
X(xbe_other, "This folder holds another Xbox game (\"%s\").", "Esta carpeta contiene otro juego de Xbox (\"%s\").")
X(files_not_found, "Game files not found", "No se encontraron los archivos del juego")
X(install_game, "Install the game", "Instalar el juego")
X(fix_game_page, "Fix it on the Game page", "Corregir en la página Juego")
X(fix_options, "Fix it in Options", "Corregir en Opciones")
X(cannot_find_exe, "The launcher cannot find %s.", "El lanzador no encuentra %s.")
X(choose_program, "Choose the game program", "Elegir el programa del juego")
X(install_detail,
  "Install the game from an Xbox disc image (.iso), or choose the folder with the game files.",
  "Instala el juego desde una imagen de disco de Xbox (.iso) o elige la carpeta con los archivos del juego.")
X(save_not_found, "Save folder not found", "No se encontró la carpeta de partidas")
X(save_not_exist, "The chosen save folder does not exist.", "La carpeta de partidas elegida no existe.")
X(texpack_not_found, "Texture pack not found", "No se encontró el paquete de texturas")
X(texpack_not_exist,
  "The chosen texture pack folder does not exist.",
  "La carpeta del paquete de texturas elegida no existe.")
X(sndpack_not_found, "Sound pack not found", "No se encontró el paquete de sonidos")
X(sndpack_not_exist,
  "The chosen sound pack folder does not exist.",
  "La carpeta del paquete de sonidos elegida no existe.")
X(keymap_not_found, "Key map file not found", "No se encontró el archivo de teclas")
X(keymap_not_exist, "The chosen key map file does not exist.", "El archivo de teclas elegido no existe.")
X(browse, "Browse...", "Examinar...")
X(game_program, "Game program", "Programa del juego")
X(path_to, "path to %s", "ruta a %s")
X(choose_x, "Choose %s", "Elegir %s")
X(found, "Found", "Encontrado")
X(not_chosen, "Not chosen", "Sin elegir")
X(not_found, "Not found", "No encontrado")
X(exe_hint_wizard,
  "The port's program, %s. It is found automatically when it is next to the launcher.",
  "El programa del port, %s. Se encuentra solo cuando está junto al lanzador.")
X(your_game_copy, "Your game copy", "Tu copia del juego")
X(folder_with_xbe, "folder that contains default.xbe", "carpeta que contiene default.xbe")
X(choose_copy_folder, "Choose the folder with your game copy", "Elige la carpeta con tu copia del juego")
X(folder_not_exist, "This folder does not exist.", "Esta carpeta no existe.")
X(disc_hint_wizard,
  "Choose the folder with the game files (it holds default.xbe).",
  "Elige la carpeta con los archivos del juego (contiene default.xbe).")
X(file_not_exist, "This file does not exist.", "Este archivo no existe.")
X(extract_not_started, "The extraction could not be started.", "No se pudo iniciar la extracción.")
X(extract_stopped,
  "Stopped. Choose \"Extract the game\" again to continue where it stopped.",
  "Detenida. Elige \"Extraer el juego\" otra vez para seguir donde se detuvo.")
X(extract_failed, "The extraction failed: %s", "La extracción falló: %s")
X(extract_done, "Done. Your game copy is ready in %s.", "Listo. Tu copia del juego está en %s.")
X(no_game_files,
  "Choose an Xbox disc image of The Warriors: the launcher copies the game's files out of it into a folder, once. The image itself is only read and is left untouched.",
  "Elige una imagen de disco de Xbox de The Warriors: el lanzador copia los archivos del juego a una carpeta, una sola vez. La imagen solo se lee y no se modifica.")
X(disc_image, "Disc image", "Imagen de disco")
X(your_disc_image, "your game disc image (.iso)", "tu imagen de disco del juego (.iso)")
X(choose_disc_image, "Choose your game disc image", "Elige tu imagen de disco del juego")
X(iso_summary, "The Warriors (Xbox): %d files, %s.", "The Warriors (Xbox): %d archivos, %s.")
X(iso_other,
  "This disc image holds another Xbox game (\"%s\"), not The Warriors.",
  "Esta imagen de disco contiene otro juego de Xbox (\"%s\"), no The Warriors.")
X(iso_no_xbe,
  "This disc image has no default.xbe: it is not an Xbox game disc.",
  "Esta imagen de disco no tiene default.xbe: no es un disco de juego de Xbox.")
X(iso_xbe_unreadable,
  "The default.xbe in this disc image cannot be read.",
  "No se puede leer el default.xbe de esta imagen de disco.")
X(extract_to, "Extract to", "Extraer en")
X(folder_for_files, "folder for the game files", "carpeta para los archivos del juego")
X(choose_dest, "Choose where to put the game files", "Elige dónde guardar los archivos del juego")
X(default_dir, "Default", "Predeterminado")
X(cancel, "Cancel", "Cancelar")
X(stopping, "Stopping...", "Deteniendo...")
X(iso_progress,
  "%d of %d files, %s of %s copied. This takes a few minutes.",
  "%d de %d archivos, %s de %s copiados. Esto tarda unos minutos.")
X(extract_game, "Extract the game", "Extraer el juego")
X(free_on_drive, "%s free on that drive.", "%s libres en esa unidad.")
X(free_unknown, "free space unknown.", "espacio libre desconocido.")
X(already_all,
  "This folder already holds every file of the game.",
  "Esta carpeta ya tiene todos los archivos del juego.")
X(not_enough_space, "Not enough free space: needs %s, only %s", "No hay espacio suficiente: necesita %s, solo hay %s")
X(needs_space, "Needs %s of free space; %s", "Necesita %s de espacio libre; %s")
X(use_this_folder, "Use this folder", "Usar esta carpeta")
X(preset_laptop, "Performance", "Rendimiento")
X(preset_iris_xe, "Iris Xe", "Iris Xe")
X(preset_balanced, "Balanced", "Equilibrado")
X(preset_gaming, "Quality", "Calidad")
X(preset_custom, "Custom", "Personalizado")
X(preset_laptop_desc,
  "Smoothness first: Full HD and sharp textures without heavy effects. For integrated graphics and older cards.",
  "Primero la fluidez: Full HD y texturas nítidas sin efectos pesados. Para gráficos integrados y tarjetas antiguas.")
X(preset_iris_xe_desc,
  "FSR upscaling from 720p pays for the lights, bloom, better rain and soft shadows at 60 FPS. For Intel Iris Xe and Arc integrated graphics.",
  "El escalado FSR desde 720p paga las luces, el resplandor, la mejor lluvia y las sombras suaves a 60 FPS. Para gráficos integrados Intel Iris Xe y Arc.")
X(preset_balanced_desc,
  "A bit of everything: light anti-aliasing, soft shadows, glowing lights and better rain. For entry-level graphics cards.",
  "Un poco de todo: antialiasing ligero, sombras suaves, luces con resplandor y mejor lluvia. Para tarjetas gráficas básicas.")
X(preset_gaming_desc,
  "Picture first: anti-aliasing, soft detailed shadows, glowing lights and ambient occlusion. For dedicated graphics cards.",
  "Primero la imagen: antialiasing, sombras suaves y detalladas, luces con resplandor y oclusión ambiental. Para tarjetas gráficas dedicadas.")
/* Why the first run picked a preset (gpu.h gpu_reason): preset, card name, memory. */
X(gpu_picked, "We picked %s for your %s (%s).", "Elegimos %s para tu %s (%s).")
X(gpu_picked_integrated, "We picked %s for your %s (integrated graphics).", "Elegimos %s para tu %s (gráfica integrada).")
X(gpu_picked_no_memory, "We picked %s: we do not know how much memory your %s has.",
  "Elegimos %s: no sabemos cuánta memoria tiene tu %s.")
X(gpu_picked_unknown, "We could not identify your graphics card, so we picked %s.",
  "No pudimos identificar tu tarjeta gráfica, así que elegimos %s.")
/* The detected card and the preset it recommends (gpu.h gpu_detected_line): card (with memory), preset. */
X(gpu_detected, "Detected: %s — recommended: %s", "Detectamos: %s — recomendado: %s")
/* While the card is probed (on a worker, gui.h app_gpu_start) and the texture packs are measured. */
X(gpu_detecting, "Detecting your graphics card...", "Detectando tu tarjeta gráfica...")
X(vram_measuring_packs, "Measuring the texture packs...", "Midiendo los paquetes de texturas...")
X(gpu_detected_none, "We could not identify your graphics card — recommended: %s",
  "No pudimos identificar tu tarjeta gráfica — recomendado: %s")
/* The video-memory meter on the Display page (vram.h): used, available (formatted "1,5 GB"). */
X(vram_title, "Video memory", "Memoria de video")
X(vram_used, "Used by these settings: %s / Available: %s", "Usado por estos ajustes: %s / Disponible: %s")
X(vram_used_only, "Used by these settings: %s", "Usado por estos ajustes: %s")
X(vram_probe_failed, "Could not read the graphics card's memory", "No se pudo leer la memoria de la placa")
X(vram_shared, "Integrated graphics: this memory is shared with the system.",
  "Gráfica integrada: esta memoria se comparte con el sistema.")
X(vram_over, "These settings exceed your graphics card's memory: the game may run slowly or close.",
  "Estos ajustes superan la memoria de tu placa: puede ir lento o cerrarse.")
X(vram_cat_screen, "Resolution and effects", "Resolución y efectos")
X(vram_cat_shadows, "Shadows", "Sombras")
X(vram_cat_textures, "HD textures", "Texturas HD")
X(vram_cat_characters, "HD characters", "Personajes HD")
X(vram_cat_other, "Other", "Otros")
X(vram_estimate_note, "An estimate: the HD textures fill their share as you play.",
  "Es una estimación: las texturas HD ocupan su parte a medida que juegas.")
X(preset_custom_desc, "Your own combination of the settings below.", "Tu propia combinación de los ajustes de abajo.")
X(rename_profile, "Rename profile", "Renombrar perfil")
X(name_invalid, "The name is empty, already used, or contains ']'.", "El nombre está vacío, ya existe o contiene ']'.")
X(rename, "Rename", "Renombrar")
X(delete_profile, "Delete profile", "Eliminar perfil")
X(delete_profile_q, "Delete the profile \"%s\"?", "¿Eliminar el perfil \"%s\"?")
X(delete_btn, "Delete", "Eliminar")
X(new_btn, "New", "Nuevo")
X(duplicate, "Duplicate", "Duplicar")
X(eyebrow, "NATIVE PC PORT  ·  XBOX VERSION", "PORT NATIVO PARA PC  ·  VERSIÓN DE XBOX")
X(sum_display, "DISPLAY", "PANTALLA")
X(sum_perf, "PERFORMANCE", "RENDIMIENTO")
X(sum_lang, "LANGUAGE", "IDIOMA")
X(fullscreen, "Fullscreen", "Pantalla completa")
X(windowed, "Windowed", "Ventana")
X(fps60, "60 FPS", "60 FPS")
X(fps30, "30 FPS", "30 FPS")
X(vsync_on, "V-Sync on", "V-Sync activado")
X(vsync_off, "V-Sync off", "V-Sync desactivado")
X(voices_english, "Voices stay in English", "Las voces quedan en inglés")
X(last_session, "Last game session", "Última sesión de juego")
X(running, "Running", "En marcha")
X(running_profile, "Profile \"%s\".", "Perfil \"%s\".")
X(closed_ok, "Closed normally", "Cerrado con normalidad")
X(closed_ok_desc, "The last game ended without an error.", "La última partida terminó sin errores.")
X(stopped_error, "Stopped with an error", "Se detuvo con un error")
X(exit_code, "Exit code %d. The log shows what happened.", "Código de salida %d. El registro muestra qué pasó.")
X(no_game_yet, "No game yet", "Aún no se ha jugado")
X(no_game_started, "No game started yet.", "Todavía no se inició ninguna partida.")
X(no_game_since,
  "No game started since the launcher opened.",
  "No se inició ninguna partida desde que se abrió el lanzador.")
X(open_last_log, "Open last log", "Abrir último registro")
X(close_on_play, "Close the launcher on PLAY", "Cerrar el lanzador al JUGAR")
X(close_on_play_desc, "Closes this window once the game starts.", "Cierra esta ventana cuando empieza el juego.")
X(display_sub,
  "Picture and smoothness. Changes apply the next time you start the game.",
  "Imagen y fluidez. Los cambios se aplican la próxima vez que abras el juego.")
X(preset, "Preset", "Preajuste")
X(group_image, "Image", "Imagen")
X(group_image_hint, "anti-aliasing, filtering, scaling, sharpness", "antialiasing, filtrado, escalado, nitidez")
X(group_lighting, "Lighting", "Iluminación")
X(group_lighting_hint, "street lamps, fire, neon signs, light on characters", "faroles, fuego, neones, luz en personajes")
X(group_shadows, "Shadows", "Sombras")
X(group_shadows_hint, "quality, softness, range, contact", "calidad, suavizado, alcance, contacto")
X(group_effects, "Effects", "Efectos")
X(group_effects_hint, "HD rain, bloom, film look, sky", "lluvia HD, bloom, look de película, cielo")
X(group_interface, "Interface", "Interfaz")
X(group_interface_hint, "HUD, FPS counter", "HUD, contador de FPS")
X(window_mode, "Window mode", "Modo de ventana")
X(window_mode_desc,
  "Fullscreen fills your desktop. F11 or Alt+Enter switches while playing.",
  "Pantalla completa ocupa todo el escritorio. F11 o Alt+Intro cambia el modo mientras juegas.")
X(refresh_match, "Match the screen's refresh rate to 60 fps", "Ajustar la frecuencia de la pantalla a 60 fps")
X(refresh_match_desc,
  "On a 144 Hz screen, fullscreen switches to 120 Hz so motion stays smooth. Your screen goes back when the game closes.",
  "En un monitor de 144 Hz, la pantalla completa pasa a 120 Hz para que el movimiento sea fluido. El monitor vuelve a su frecuencia al cerrar el juego.")
X(original, "Original", "Original")
X(res_screen_pill, "Screen", "Monitor")
X(res_desc, "Higher is sharper. Now %s.", "Más alta es más nítida. Ahora %s.")
X(res_desc_screen, "Higher is sharper. Now %s, your screen's.", "Más alta es más nítida. Ahora %s, la de tu monitor.")
X(resolution, "Resolution", "Resolución")
X(heavy, "Heavy on laptops.", "Exigente para portátiles.")
X(render_scale, "Render scale", "Escala de renderizado")
X(render_scale_desc,
  "The original 640 × 480 picture, drawn this many times larger.",
  "La imagen original de 640 × 480, dibujada tantas veces más grande.")
X(widescreen, "Widescreen", "Panorámica")
X(widescreen_desc,
  "Fills 16:9 screens. Off keeps the original 4:3 picture with bars at the sides.",
  "Ocupa las pantallas 16:9. Desactivada mantiene la imagen 4:3 original con barras a los lados.")
X(movie_hd, "HD movies", "Vídeos en HD")
X(movie_hd_desc,
  "Plays the disc's 1280 × 720 movies, drawn sharp at your resolution. Off plays the original 640 × 480 ones.",
  "Reproduce los vídeos de 1280 × 720 del disco, nítidos a tu resolución. Desactivado usa los originales de 640 × 480.")
X(gamma_ramp, "Game's original brightness", "Brillo original del juego")
X(gamma_ramp_desc,
  "Shows the picture as bright as on the Xbox, with the game's Brightness setting. Off shows it darker, as rendered.",
  "Muestra la imagen tan luminosa como en Xbox, con el ajuste de Brillo del juego. Desactivado la muestra más oscura, tal como se dibuja.")
X(frame_rate, "Frame rate", "Fotogramas por segundo")
X(frame_rate_30_desc, "Original mode.", "Modo original.")
X(frame_rate_60_desc, "Smoother. Some cutscenes may show small visual glitches.",
  "Más fluido. Algunas cinemáticas pueden mostrar pequeños errores visuales.")
X(vsync, "V-Sync", "V-Sync")
X(vsync_desc,
  "Waits for the monitor to avoid tearing. Off shows each frame at once.",
  "Espera al monitor para evitar cortes en la imagen. Desactivado muestra cada fotograma al instante.")
X(auto_mode, "Auto", "Auto")
X(fif, "Frames in flight", "Fotogramas en cola")
X(fif_desc,
  "More is smoother on weak graphics cards, 1 responds fastest. Auto uses 2.",
  "Más es más fluido en tarjetas gráficas modestas; 1 responde más rápido. Auto usa 2.")
X(off, "Off", "No")
X(msaa, "Anti-aliasing (MSAA)", "Antialiasing (MSAA)")
X(msaa_desc, "Smooths jagged edges.", "Suaviza los bordes dentados.")
X(supersampling, "Supersampling", "Supermuestreo")
X(supersampling_desc,
  "Draws the picture larger and scales it down: the sharpest image.",
  "Dibuja la imagen más grande y la reduce: la imagen más nítida.")
X(fsr, "AMD FSR 1 upscaling", "Escalado AMD FSR 1")
X(fsr_desc,
  "Draws the 3D scene at a lower resolution and upscales it with FSR 1: more frames per second. Replaces supersampling and sharpness while on.",
  "Dibuja la escena 3D a menor resolución y la agranda con FSR 1: más fotogramas por segundo. Mientras está activo reemplaza al supermuestreo y la nitidez.")
X(fsr_ultra, "Ultra quality", "Ultra calidad")
X(fsr_quality, "Quality", "Calidad")
X(fsr_balanced, "Balanced", "Equilibrado")
X(fsr_performance, "Performance", "Rendimiento")
X(texfilter, "Texture filtering", "Filtrado de texturas")
X(texfilter_desc,
  "Keeps floors and walls sharp at a distance. Light on any PC.",
  "Mantiene nítidos suelos y paredes a distancia. Ligero en cualquier PC.")
X(shadow_normal, "Normal", "Normal")
X(shadow_high, "High", "Alta")
X(shadow_ultra, "Ultra", "Ultra")
X(shadow_quality, "Shadow quality", "Calidad de sombras")
X(shadow_quality_desc, "Sharper character shadows.", "Sombras de personajes más nítidas.")
X(soft_shadows, "Soft shadows", "Sombras suaves")
X(soft_shadows_desc,
  "Blurs the outline of character shadows.",
  "Difumina el contorno de las sombras de los personajes.")
X(shadow_range, "Shadow range", "Alcance de las sombras")
X(shadow_range_desc,
  "Character shadows no longer fall on floors far below them.",
  "Las sombras de los personajes ya no caen en suelos muy por debajo de ellos.")
X(hud_original_desc, "The game's own health bars, unchanged.", "Las barras de salud del juego, sin cambios.")
X(hud_hd_desc, "Cleaner, thicker bars with a dark outline.", "Barras más limpias y gruesas con contorno oscuro.")
X(hud_hdglass_desc,
  "HD bars on frosted glass: see-through, yet easy to read. Recommended.",
  "Barras HD sobre vidrio esmerilado: translúcidas y fáciles de leer. Recomendado.")
X(hud_glass_desc,
  "The original bar on a frosted glass panel.",
  "La barra original sobre un panel de vidrio esmerilado.")
X(hud_glass, "Glass", "Vidrio")
X(hud_hdglass, "HD Glass", "HD Vidrio")
X(health_bars, "Health bars", "Barras de salud")
X(health_bars_desc, "How the health bars look on screen.", "Cómo se ven las barras de salud en pantalla.")
X(customize_bars, "Customize the bars", "Personalizar las barras")
X(customize_bars_desc, "Opacity, frost and height of the HD bars.", "Opacidad, esmerilado y altura de las barras HD.")
X(empty_opacity, "Empty part opacity", "Opacidad de la parte vacía")
X(empty_opacity_desc, "How visible the empty part of a bar is.", "Qué tan visible es la parte vacía de una barra.")
X(fill_opacity, "Fill opacity", "Opacidad del relleno")
X(fill_opacity_desc, "How solid the coloured part of a bar is.", "Qué tan sólida es la parte de color de una barra.")
X(frost, "Frost", "Esmerilado")
X(frost_desc,
  "Blurs the scene behind the bar. 0 turns it off.",
  "Difumina la escena detrás de la barra. 0 lo desactiva.")
X(height, "Height", "Altura")
X(height_desc, "Makes the bars thicker or thinner.", "Hace las barras más gruesas o más finas.")
X(reset, "Reset", "Restablecer")
X(reset_desc, "Back to the style's own look.", "Vuelve al aspecto propio del estilo.")
X(reset_bars, "Reset the bars", "Restablecer las barras")
X(game_sub,
  "Your game files, language and optional add-ons.",
  "Tus archivos del juego, idioma y complementos opcionales.")
X(game_files, "Game files", "Archivos del juego")
X(installed, "The Warriors (Xbox) is installed", "The Warriors (Xbox) está instalado")
X(hide_iso, "Hide the disc image options", "Ocultar las opciones de imagen de disco")
X(install_iso, "Install from a disc image (.iso)...", "Instalar desde una imagen de disco (.iso)...")
X(open_folder, "Open folder", "Abrir carpeta")
X(run_wizard, "Run the setup wizard", "Abrir el asistente de configuración")
X(exe_desc,
  "The port's program, %s. Found automatically next to the launcher.",
  "El programa del port, %s. Se encuentra solo junto al lanzador.")
X(game_folder, "Game folder", "Carpeta del juego")
X(game_folder_desc,
  "The folder with the game files (it holds default.xbe).",
  "La carpeta con los archivos del juego (contiene default.xbe).")
X(save_folder, "Save folder", "Carpeta de partidas")
X(save_folder_desc,
  "Your saved games. Empty uses the hdd folder next to the game.",
  "Tus partidas guardadas. Vacía usa la carpeta hdd junto al juego.")
X(save_default_hint, "default: the hdd folder next to the game", "predeterminada: la carpeta hdd junto al juego")
X(choose_save, "Choose the save folder", "Elige la carpeta de partidas")
X(lang_text, "Language and text", "Idioma y texto")
X(game_language, "Game language", "Idioma del juego")
X(game_language_desc,
  "Menus and on-screen text. The voices stay in English.",
  "Menús y textos en pantalla. Las voces quedan en inglés.")
X(fix_typos, "Fix translation errors", "Corregir errores de traducción")
X(fix_typos_desc,
  "Spanish game text only: e.g. the gym's \"Sentadillas\" becomes \"Abdominales\".",
  "Solo para el juego en español: p. ej. \"Sentadillas\" del gimnasio pasa a \"Abdominales\".")
X(spray, "Spray can duration", "Duración del aerosol")
X(spray_desc,
  "How long a spray can lasts in the graffiti mini-game. 100% is the original.",
  "Cuánto dura un aerosol en el minijuego de grafitis. 100% es el original.")
/* Skip intro videos (Game, every preset) */
X(skip_intro, "Skip intro videos", "Saltar videos de inicio")
X(skip_intro_desc,
  "Start at the title screen without the logo and opening movies. The movies of the story levels and the title screen's demo still play.",
  "Empieza en la pantalla de título sin los logotipos ni el video de apertura. Los videos de los niveles de la historia y la demo de la pantalla de título se siguen viendo.")
/* Sound volumes (Game page) */
X(sound_card, "Sound", "Sonido")
X(dialogue_volume, "Dialogue volume", "Volumen de los diálogos")
X(dialogue_volume_desc,
  "Louder or quieter voices: the crew, enemies, passers-by and the story lines, also in the cutscenes made with the game's engine. 0 dB is the original mix. The movies keep their own sound.",
  "Voces más fuertes o más bajas: la banda, los enemigos, los transeúntes y las frases de la historia, también en las escenas hechas con el motor del juego. 0 dB es la mezcla original. Los videos conservan su propio sonido.")
X(music_volume, "Music volume", "Volumen de la música")
X(music_volume_desc,
  "Added to the game's own music setting. 0 dB is the original mix.",
  "Se suma al ajuste de música del propio juego. 0 dB es la mezcla original.")
X(sfx_volume, "Effects volume", "Volumen de los efectos")
X(sfx_volume_desc,
  "Every other game sound: hits, steps, cars, ambience. Added to the game's own effects setting. 0 dB is the original mix.",
  "Todos los demás sonidos del juego: golpes, pasos, coches, ambiente. Se suma al ajuste de efectos del propio juego. 0 dB es la mezcla original.")
X(addons, "Add-ons", "Complementos")
X(pack_optional, "Optional. See the repository to make one.", "Opcional. Consulta el repositorio para crear uno.")
X(texture_pack, "Texture pack", "Paquete de texturas")
X(no_texture_pack, "no texture pack", "sin paquete de texturas")
X(choose_texpack, "Choose the texture pack folder", "Elige la carpeta del paquete de texturas")
X(sound_pack, "Sound pack", "Paquete de sonidos")
X(no_sound_pack, "no sound pack", "sin paquete de sonidos")
X(choose_sndpack, "Choose the sound pack folder", "Elige la carpeta del paquete de sonidos")
X(button_icons, "Button icons", "Iconos de botones")
X(button_icons_desc,
  "What the game's button prompts show. Auto follows the device you used last.",
  "Lo que muestran los avisos de botones del juego. Auto sigue el último dispositivo que usaste.")
X(icons_auto, "Auto", "Auto")
X(icons_kbm, "Keyboard & mouse", "Teclado y mouse")
X(pack_not_found, "Pack not found.", "No se encontró el paquete.")
X(glyph_pack, "Button-icon pack", "Paquete de iconos de botones")
X(glyph_pack_hint, "button-icon pack folder", "carpeta del paquete de iconos de botones")
X(choose_glyphs, "Choose the button-icon pack folder", "Elige la carpeta del paquete de iconos de botones")
X(extra_args, "Extra arguments", "Argumentos adicionales")
X(extra_args_desc,
  "Added to the game's command line as typed. Every option is listed in docs/ARGUMENTS.md in the repository.",
  "Se agregan tal cual a la línea de comandos del juego. Todas las opciones están en docs/ARGUMENTS.md del repositorio.")
X(extra_args_hint, "e.g. --stats", "p. ej. --stats")
X(options_sub,
  "Profiles, the launcher itself and technical options.",
  "Perfiles, el propio lanzador y opciones técnicas.")
X(profiles, "Profiles", "Perfiles")
X(profile, "Profile", "Perfil")
X(profile_desc,
  "A separate set of settings, for example one per PC.",
  "Un conjunto de ajustes aparte, por ejemplo uno por PC.")
X(manage, "Manage", "Administrar")
X(manage_desc,
  "New starts from the recommended settings and keeps the game files.",
  "Nuevo parte de los ajustes recomendados y conserva los archivos del juego.")
X(launcher, "Launcher", "Lanzador")
X(ui_language, "Launcher language", "Idioma del lanzador")
X(ui_language_desc,
  "The language of this window. The game's language is on the Game page.",
  "El idioma de esta ventana. El idioma del juego está en la página Juego.")
X(show_advanced, "Show advanced options", "Mostrar opciones avanzadas")
X(show_advanced_desc,
  "Renderer, audio, diagnostics and developer options, below.",
  "Renderizador, audio, diagnóstico y opciones de desarrollo, abajo.")
X(rendering, "Rendering", "Renderizado")
X(none, "None", "Ninguno")
X(immediate, "Immediate", "Inmediato")
X(renderer, "Renderer", "Renderizador")
X(renderer_desc, "None keeps the CPU-only path (testing).", "Ninguno usa solo la CPU (pruebas).")
X(present_mode, "Present mode", "Modo de presentación")
X(present_mode_desc,
  "Auto: mailbox, else FIFO (V-Sync). Immediate is V-Sync off.",
  "Auto: mailbox o, si no, FIFO (V-Sync). Inmediato es V-Sync desactivado.")
X(res_typed, "Resolution (WxH)", "Resolución (AnchoxAlto)")
X(res_typed_desc,
  "Any size, typed. Empty: 640 × 480 × render scale.",
  "Cualquier tamaño, escrito. Vacío: 640 × 480 × escala.")
X(render_targets, "Render targets", "Render targets")
X(render_targets_desc, "Off disables character shadows.", "Desactivado quita las sombras de los personajes.")
X(backends, "Audio, video and diagnostics", "Audio, video y diagnóstico")
X(audio, "Audio", "Audio")
X(audio_desc, "None runs without sound.", "Ninguno funciona sin sonido.")
X(video, "Video", "Video")
X(video_desc,
  "The window backend. None runs without a window.",
  "El sistema de ventanas. Ninguno funciona sin ventana.")
X(frame_stats, "Frame statistics", "Estadísticas de fotogramas")
X(frame_stats_desc, "--stats: frame times in the log.", "--stats: tiempos de fotograma en el registro.")
X(load_profile, "Load times", "Tiempos de carga")
X(load_profile_desc, "--load-profile: where level load time goes, in the log.",
  "--load-profile: en qué se va el tiempo de carga de cada nivel, en el registro.")
X(trace_log, "Trace log", "Registro de trazas")
X(trace_log_desc, "--trace: every call in the log. Slow.", "--trace: cada llamada en el registro. Lento.")
X(developer, "Developer", "Desarrollo")
X(frame_lock, "Frame-locked clock", "Reloj ligado a fotogramas")
X(frame_lock_desc, "The game's clock follows the frames.", "El reloj del juego sigue a los fotogramas.")
X(rate_fixes, "Mini-game rate fixes", "Corrección de velocidad de minijuegos")
X(rate_fixes_desc, "Keeps the mini-games at their original speed.", "Mantiene los minijuegos a su velocidad original.")
X(spawn_scale, "Spawn scale", "Escala de aparición")
X(spawn_scale_desc, "Experiment: more people in the streets.", "Experimento: más gente en las calles.")
X(warp, "Warp to mission", "Saltar a una misión")
X(warp_desc,
  "Loading a save starts that story mission (1-23). Saving is off for that session.",
  "Al cargar una partida empieza esa misión de la historia (1-23). No se guarda en esa sesión.")
X(warp_hint, "e.g. 8 or 8:3 (empty = off)", "p. ej. 8 u 8:3 (vacío = desactivado)")
X(key_map, "Key map file", "Archivo de teclas")
X(key_map_desc, "Optional --input-config file.", "Archivo --input-config opcional.")
X(no_key_map, "no key map file", "sin archivo de teclas")
X(choose_key_map, "Choose the key map file", "Elige el archivo de teclas")
X(about, "About", "Acerca de")
X(about_1,
  "The Warriors launcher: runs the Xbox game with this port's runtime.",
  "Lanzador de The Warriors: ejecuta el juego de Xbox con el runtime de este port.")
X(about_2,
  "No game files are included or downloaded. The background is made on this PC from your installed game's boot splash (or its title image) and kept in your cache folder.",
  "No se incluye ni se descarga ningún archivo del juego. El fondo se crea en este PC a partir de la pantalla de inicio de tu juego instalado (o de su imagen de título) y se guarda en tu carpeta de caché.")
X(about_3,
  "Interface font: Inter (SIL Open Font License). User interface: Dear ImGui, SDL2.",
  "Fuente de la interfaz: Inter (SIL Open Font License). Interfaz de usuario: Dear ImGui, SDL2.")
X(bg_plain_no_game, "Background: plain until your game is installed.", "Fondo: liso hasta que instales el juego.")
X(bg_plain_err, "Background: plain (no art could be read: %s).", "Fondo: liso (no se pudo leer ninguna imagen: %s).")
X(bg_plain_sdl, "Background: plain (%s).", "Fondo: liso (%s).")
X(bg_splash, "Background: the boot splash of your game, %d × %d.", "Fondo: la pantalla de inicio de tu juego, %d × %d.")
X(bg_title, "Background: the title image of your game, %d × %d.", "Fondo: la imagen de título de tu juego, %d × %d.")
X(step_program, "Game program", "Programa del juego")
X(step_copy, "Game copy", "Copia del juego")
X(step_save, "Save folder", "Partidas")
X(step_graphics, "Graphics", "Gráficos")
X(welcome, "Welcome", "Bienvenido")
X(welcome_sub, "Four quick steps and you are ready to play.", "Cuatro pasos rápidos y listo para jugar.")
X(wiz_program,
  "The launcher looks for %s next to itself and one folder up. If it was not found, choose it here.",
  "El lanzador busca %s junto a sí mismo y una carpeta más arriba. Si no lo encontró, elígelo aquí.")
X(wiz_iso_card, "Choose your game disc image (.iso)", "Elige tu imagen de disco del juego (.iso)")
X(wiz_iso_card_desc, "Recommended: most players have this.", "Recomendado: es lo que tiene la mayoría.")
X(wiz_folder_card, "I already have an extracted folder", "Ya tengo una carpeta extraída")
X(wiz_folder_card_desc,
  "A folder with default.xbe, copied from your disc.",
  "Una carpeta con default.xbe, copiada de tu disco.")
X(copy_in_use, "Game copy in use: %s", "Copia del juego en uso: %s")
X(wiz_save,
  "Your saved games are kept in this folder. The default, the hdd folder next to the game, suits most players. It is created when it does not exist yet.",
  "Tus partidas se guardan en esta carpeta. La predeterminada, la carpeta hdd junto al juego, sirve para la mayoría. Se crea si todavía no existe.")
X(folder_for_saves, "folder for your saved games", "carpeta para tus partidas guardadas")
X(will_be_created, "Will be created", "Se creará")
X(use_default, "Use the default", "Usar la predeterminada")
X(wiz_graphics,
  "Pick what fits your PC. You can change this, and every single setting, later on the Display page.",
  "Elige lo que mejor va con tu PC. Puedes cambiar esto, y cada ajuste, después en la página Pantalla.")
X(skip_setup, "Skip setup", "Omitir configuración")
X(back, "Back", "Atrás")
X(finish, "Finish", "Terminar")
X(next, "Next", "Siguiente")
X(save_cannot_create, "The save folder %s cannot be created.", "No se puede crear la carpeta de partidas %s.")
X(wiz_pick_preset, "Pick one of the presets to finish.", "Elige uno de los preajustes para terminar.")
X(wiz_wait, "Wait for the extraction to finish.", "Espera a que termine la extracción.")
X(wiz_choose_iso,
  "Choose your disc image and extract the game to continue.",
  "Elige tu imagen de disco y extrae el juego para continuar.")
X(wiz_fix, "Fix the field above to continue.", "Corrige el campo de arriba para continuar.")
/* Controls page */
X(controls_sub,
  "Keyboard and mouse keys, the controller and the in-game button icons.",
  "Teclas del teclado y el mouse, el control y los iconos de botones del juego.")
X(kbm_card, "Keyboard & mouse", "Teclado y mouse")
X(kbm_help,
  "Click a key to change it, then press a key or mouse button (Esc cancels). Right-click a key to clear it.",
  "Haz clic en una tecla para cambiarla y luego presiona una tecla o un botón del mouse (Esc cancela). Haz clic derecho en una tecla para quitarla.")
X(key_file, "Saved to %s", "Se guarda en %s")
X(key_file_new, "Defaults (no key map file yet); changes go to %s", "Predeterminado (aún no hay archivo de teclas); los cambios van a %s")
X(key_file_none, "Choose the game program on the Game page to save changes.", "Elige el programa del juego en la página Juego para guardar los cambios.")
X(key_file_warn, "%d lines of the key map file were ignored.", "Se ignoraron %d líneas del archivo de teclas.")
X(reset_defaults, "Reset to defaults", "Restablecer valores predeterminados")
X(reset_done, "All keys are back to the defaults.", "Todas las teclas volvieron a los valores predeterminados.")
X(press_key, "Press a key or mouse button for %s... (Esc cancels)", "Presiona una tecla o un botón del mouse para %s... (Esc cancela)")
X(press_short, "Press...", "Presiona...")
X(key_reserved, "Esc and F1 are reserved (mouse capture and quit).", "Esc y F1 están reservadas (captura del mouse y salir).")
X(key_moved, "%s was bound to %s; it now does %s.", "%s estaba asignada a %s; ahora hace %s.")
X(key_cleared, "%s no longer does anything.", "%s ya no hace nada.")
X(not_bound, "Not bound", "Sin asignar")
X(key_space, "Space", "Espacio")
X(key_enter, "Enter", "Enter")
X(key_backspace, "Backspace", "Retroceso")
X(key_up, "Up", "Arriba")
X(key_down, "Down", "Abajo")
X(key_left, "Left", "Izquierda")
X(key_right, "Right", "Derecha")
X(key_shift, "Shift", "Mayús")
X(key_rshift, "Right Shift", "Mayús derecha")
X(key_rctrl, "Right Ctrl", "Ctrl derecho")
X(key_ralt, "Right Alt", "Alt derecho")
X(mouse_left, "Left", "Izquierdo")
X(mouse_middle, "Middle", "Central")
X(mouse_right, "Right", "Derecho")
X(mouse_back, "Back", "Atrás")
X(mouse_forward, "Forward", "Adelante")
X(save_failed, "Could not save the key map: %s", "No se pudo guardar el archivo de teclas: %s")
X(group_move, "Movement", "Movimiento")
X(group_camera, "Camera (the mouse also moves it)", "Cámara (el mouse también la mueve)")
X(group_actions, "Actions", "Acciones")
X(group_menus, "Menus and D-pad", "Menús y cruceta")
X(act_ls_up, "Move forward", "Avanzar")
X(act_ls_down, "Move back", "Retroceder")
X(act_ls_left, "Move left", "Moverse a la izquierda")
X(act_ls_right, "Move right", "Moverse a la derecha")
X(act_rs_up, "Camera up", "Cámara arriba")
X(act_rs_down, "Camera down", "Cámara abajo")
X(act_rs_left, "Camera left", "Cámara a la izquierda")
X(act_rs_right, "Camera right", "Cámara a la derecha")
X(act_x, "Light attack (X)", "Ataque ligero (X)")
X(act_a, "Heavy attack / select (A)", "Ataque fuerte / seleccionar (A)")
X(act_b, "Grab (B)", "Agarrar (B)")
X(act_y, "Pick up / climb (Y)", "Recoger / trepar (Y)")
X(act_lt, "Left trigger (LT)", "Gatillo izquierdo (LT)")
X(act_rt, "Right trigger (RT)", "Gatillo derecho (RT)")
X(act_black, "Black button", "Botón negro")
X(act_white, "White button", "Botón blanco")
X(act_lthumb, "Left stick click (L3)", "Clic del stick izquierdo (L3)")
X(act_rthumb, "Right stick click (R3)", "Clic del stick derecho (R3)")
X(act_spin, "Spin the left stick (car theft)", "Girar el stick izquierdo (robo de autos)")
X(act_start, "Pause / Start", "Pausa / Start")
X(act_back, "Back", "Back")
X(act_up, "D-pad up", "Cruceta arriba")
X(act_down, "D-pad down", "Cruceta abajo")
X(act_left, "D-pad left", "Cruceta izquierda")
X(act_right, "D-pad right", "Cruceta derecha")
X(pad_card, "Controller", "Control")
X(pad_none, "No controller detected. Connect one to see it here.", "No se detectó ningún control. Conecta uno para verlo aquí.")
X(pad_live, "Press buttons and move the sticks to test it.", "Presiona botones y mueve los sticks para probarlo.")
X(vibration, "Vibration", "Vibración")
X(vibration_desc, "Controller rumble in the game.", "Vibración del control en el juego.")
X(test_vibration, "Test", "Probar")
X(pad_remap_note,
  "Buttons or triggers in the wrong place? Set up this controller: the launcher and the game will use your layout.",
  "¿Botones o gatillos en el lugar equivocado? Configura este control: el lanzador y el juego usarán tu distribución.")
/* Controllers card (Controls page): every connected controller, its name and player */
X(pads_card, "Controllers", "Controles conectados")
X(pads_desc,
  "Each controller keeps its own buttons. Choose who is Player 1 to 4; on Auto a controller takes the next free player in the order they were connected.",
  "Cada control guarda sus propios botones. Elige quién es el jugador 1 a 4; en Automático un control toma el siguiente jugador libre según el orden en que se conectaron.")
X(pad_player_auto, "Auto", "Automático")
X(pad_player_n, "Player %d", "Jugador %d")
X(pad_setup, "Set up buttons", "Configurar botones")
X(pad_test, "Test", "Probar")
X(pad_absent, "Not connected", "No conectado")
X(pad_not_controller, "Needs a set-up", "Necesita configuración")
X(pad_shown_below, "Shown below", "Se muestra abajo")
X(pad_same_model,
  "Same model as \"%s\": both use the same buttons.",
  "Mismo modelo que \"%s\": los dos usan los mismos botones.")
X(pads_note,
  "Names and players are saved on this computer for every profile. A controller you plug in during the game goes back to its player.",
  "Los nombres y los jugadores se guardan en este equipo para todos los perfiles. Un control que conectes durante la partida vuelve a su jugador.")
/* Controller set-up (Controls page) */
X(padmap_start, "Set up this controller", "Configurar este control")
X(padmap_unknown,
  "SDL does not recognize this controller. Set it up to use it in the game.",
  "SDL no reconoce este control. Configúralo para usarlo en el juego.")
X(padmap_custom, "Custom mapping", "Asignación personalizada")
X(padmap_reset, "Reset to default", "Restablecer predeterminado")
X(padmap_reset_done, "The controller is back to its default layout.", "El control volvió a su distribución predeterminada.")
X(padmap_reset_restart,
  "Your layout was removed. Restart the launcher to see the default one.",
  "Se quitó tu distribución. Reinicia el lanzador para ver la predeterminada.")
X(padmap_no_program,
  "Choose the game program on the Game page first: the layout is saved next to it.",
  "Primero elige el programa del juego en la página Juego: la distribución se guarda junto a él.")
X(padmap_title, "Setting up %s", "Configurando %s")
X(padmap_step, "Step %d of %d", "Paso %d de %d")
X(padmap_release, "Let go of all buttons and sticks...", "Suelta todos los botones y sticks...")
X(padmap_last, "Last: %s = %s", "Último: %s = %s")
X(padmap_dup, "That input is already used for %s. Press another one.", "Esa entrada ya está asignada a %s. Presiona otra.")
X(padmap_skip, "Skip", "Omitir")
X(padmap_esc, "Esc cancels.", "Esc cancela.")
X(padmap_done, "All set. Save this layout?", "Listo. ¿Guardas esta distribución?")
X(padmap_done_desc,
  "It is saved in gamecontrollerdb.txt next to the game program; the launcher and the game use it.",
  "Se guarda en gamecontrollerdb.txt junto al programa del juego; el lanzador y el juego la usan.")
X(padmap_save, "Save layout", "Guardar distribución")
X(padmap_saved, "Saved: %s now uses your layout.", "Guardado: %s ahora usa tu distribución.")
X(padmap_lost, "The controller was disconnected; setup cancelled.", "Se desconectó el control; se canceló la configuración.")
X(padmap_raw_button, "button %d", "botón %d")
X(padmap_raw_axis, "axis %d", "eje %d")
X(padmap_raw_hat, "hat %d", "cruceta %d")
X(pm_a, "Press A (the bottom face button)", "Presiona A (el botón de abajo)")
X(pm_b, "Press B (the right face button)", "Presiona B (el botón de la derecha)")
X(pm_x, "Press X (the left face button)", "Presiona X (el botón de la izquierda)")
X(pm_y, "Press Y (the top face button)", "Presiona Y (el botón de arriba)")
X(pm_lb, "Press LB (the left bumper)", "Presiona LB (el botón superior izquierdo)")
X(pm_rb, "Press RB (the right bumper)", "Presiona RB (el botón superior derecho)")
X(pm_lt, "Pull LT (the left trigger) all the way, then let go", "Aprieta LT (el gatillo izquierdo) hasta el fondo y suéltalo")
X(pm_rt, "Pull RT (the right trigger) all the way, then let go", "Aprieta RT (el gatillo derecho) hasta el fondo y suéltalo")
X(pm_back, "Press Back / View", "Presiona Back / Vista")
X(pm_start, "Press Start / Menu", "Presiona Start / Menú")
X(pm_guide, "Press Guide (the Xbox button), or Skip if there is none", "Presiona Guía (el botón Xbox), u Omitir si no hay")
X(pm_l3, "Click the left stick (L3)", "Presiona el stick izquierdo (L3)")
X(pm_r3, "Click the right stick (R3)", "Presiona el stick derecho (R3)")
X(pm_up, "Press D-pad up", "Presiona arriba en la cruceta")
X(pm_down, "Press D-pad down", "Presiona abajo en la cruceta")
X(pm_left, "Press D-pad left", "Presiona izquierda en la cruceta")
X(pm_right, "Press D-pad right", "Presiona derecha en la cruceta")
X(pm_lx, "Push the left stick to the right, then let go", "Mueve el stick izquierdo a la derecha y suéltalo")
X(pm_ly, "Push the left stick up, then let go", "Mueve el stick izquierdo hacia arriba y suéltalo")
X(pm_rx, "Push the right stick to the right, then let go", "Mueve el stick derecho a la derecha y suéltalo")
X(pm_ry, "Push the right stick up, then let go", "Mueve el stick derecho hacia arriba y suéltalo")
X(pm_guide_short, "Guide", "Guía")
X(pm_lx_short, "Left stick left/right", "Stick izquierdo (izquierda/derecha)")
X(pm_ly_short, "Left stick up/down", "Stick izquierdo (arriba/abajo)")
X(pm_rx_short, "Right stick left/right", "Stick derecho (izquierda/derecha)")
X(pm_ry_short, "Right stick up/down", "Stick derecho (arriba/abajo)")
X(icons_card, "In-game button icons", "Iconos de botones en el juego")
X(icons_kbm_note,
  "Keyboard & mouse icons are drawn from your keys above, in the game's own icon spots.",
  "Los iconos de teclado y mouse se dibujan con tus teclas de arriba, en los mismos lugares que los iconos del juego.")
/* Save backups (Options) */
X(backups, "Save backups", "Copias de tu partida")
X(backup_on, "Back up the save before PLAY", "Copiar la partida antes de JUGAR")
X(backup_on_desc,
  "Keeps a copy of your progress each time the game starts. An unchanged save is not copied again.",
  "Guarda una copia de tu progreso cada vez que empieza el juego. Si la partida no cambió, no se vuelve a copiar.")
X(backup_count, "Backups to keep", "Copias a conservar")
X(backup_count_desc, "When there are more, the oldest ones are deleted.", "Cuando hay más, se eliminan las más antiguas.")
X(backup_list_desc,
  "Restore puts a backup back. Your current save is backed up first, so you can undo it.",
  "Restaurar vuelve a poner una copia. Antes se copia tu partida actual, así que puedes deshacerlo.")
X(backup_none, "No backups yet: the first one is made at the next PLAY.", "Todavía no hay copias: la primera se hace al próximo JUGAR.")
X(backup_detail, "%s, %d files", "%s, %d archivos")
X(backup_before_restore, "Before a restore", "Antes de restaurar")
X(backup_folder, "Backup folder", "Carpeta de copias")
X(backup_folder_desc, "Your backups stay on this computer, outside the game folder.",
  "Tus copias quedan en esta computadora, fuera de la carpeta del juego.")
X(backup_unavailable, "There is no user folder for backups (HOME or LOCALAPPDATA is not set).",
  "No hay una carpeta de usuario para las copias (HOME o LOCALAPPDATA no está definido).")
X(restore, "Restore", "Restaurar")
X(restore_title, "Restore a backup", "Restaurar una copia")
X(restore_q, "Replace your current save with the backup from %s?", "¿Reemplazar tu partida actual por la copia del %s?")
X(restore_note, "Your current save is backed up first, so you can undo this.",
  "Antes se copia tu partida actual, así que puedes deshacerlo.")
X(restore_done, "Restored the backup from %s. Your previous save is the newest backup.",
  "Se restauró la copia del %s. Tu partida anterior es la copia más reciente.")
X(restore_failed, "Restore failed: %s", "No se pudo restaurar: %s")
X(restore_running, "Close the game to restore a backup.", "Cierra el juego para restaurar una copia.")
X(backup_done, "Save backed up before PLAY (%s).", "Partida copiada antes de JUGAR (%s).")
X(backup_unchanged, "The save did not change since the last backup.", "La partida no cambió desde la última copia.")
X(backup_failed, "The save backup failed: %s", "No se pudo copiar la partida: %s")
/* Log viewer and bug report (Home) */
X(view_log, "View log", "Ver registro")
X(hide_log, "Hide log", "Ocultar registro")
X(game_log, "Game log", "Registro del juego")
X(log_of, "The last %d lines of %s.", "Las últimas %d líneas de %s.")
X(log_all, "All", "Todo")
X(log_warn, "Warnings and errors", "Advertencias y errores")
X(log_err, "Errors", "Errores")
X(log_none_level, "No lines at this level.", "No hay líneas de este nivel.")
X(refresh, "Refresh", "Actualizar")
X(copy_report, "Copy report for a bug", "Copiar informe para un error")
X(report_copied,
  "Copied: the versions, your system and graphics card, the command and the last 200 log lines, with your user name, "
  "computer name and folders replaced. Paste it into your bug report.",
  "Copiado: las versiones, tu sistema y tarjeta gráfica, el comando y las últimas 200 líneas del registro, con tu nombre "
  "de usuario, el nombre de la computadora y tus carpetas reemplazados. Pégalo en tu informe de error.")
X(about_version, "Launcher version %s.", "Versión del lanzador: %s.")
/* Sharpening (Display, Quality) */
X(sharpness, "Sharpness", "Nitidez")
X(sharpness_desc,
  "Sharpens the final image: crisper textures with clean edges.",
  "Enfoca la imagen final: texturas más nítidas con bordes limpios.")
/* Bloom (Display, Quality) */
X(bloom, "Bloom", "Resplandor")
X(bloom_desc,
  "Street lamps, neon and fire give off a soft glow. The HUD and text stay as they are.",
  "Las farolas, los neones y el fuego desprenden un brillo suave. El HUD y los textos no cambian.")
X(bloom_strength, "Bloom strength", "Intensidad del resplandor")
X(bloom_strength_desc, "How much the lights glow.", "Cuánto brillan las luces.")
/* Better rain (Display, Quality) */
X(rain_hd, "Better rain", "Lluvia mejorada")
X(rain_hd_desc,
  "When it rains, the rain falls as fine streaks at different distances and stays behind walls and people in front of it.",
  "Cuando llueve, la lluvia cae en hilos finos a distintas distancias y queda detrás de las paredes y personas que tiene delante.")
/* Puddles (Display, under Better rain; Quality) */
X(rain_puddles, "Puddles", "Charcos")
X(rain_puddles_desc,
  "When it rains, puddles form in low spots of the open street. They reflect the lights and ripple where drops land. Never under a roof.",
  "Cuando llueve, se forman charcos en las zonas bajas de la calle descubierta. Reflejan las luces y se ondulan donde caen las gotas. Nunca bajo techo.")
/* Ambient occlusion (Display, Quality) */
X(ssao, "Ambient occlusion", "Oclusión ambiental")
X(ssao_desc,
  "Corners, joints and the ground under people and objects get soft contact shading. Smoke, lights, rain and the HUD stay as they are.",
  "Las esquinas, las juntas y el suelo bajo personas y objetos reciben una sombra de contacto suave. El humo, las luces, la lluvia y el HUD no cambian.")
X(ssao_strength, "Ambient occlusion strength", "Intensidad de la oclusión ambiental")
X(ssao_strength_desc, "How dark the contact shading gets.", "Cuánto oscurece la sombra de contacto.")
/* Contact shadows (Display, Quality) */
X(contact_shadows, "Contact shadows", "Sombras de contacto")
X(contact_shadows_desc,
  "The ground right under people and objects gets a short, soft shadow, so they stand on it instead of floating. People themselves and the HUD stay as they are.",
  "El suelo justo debajo de personas y objetos recibe una sombra corta y suave, así se apoyan en él en lugar de flotar. Las personas y el HUD no cambian.")
/* Lamp lights (Display, Quality) */
X(lamp_lights, "Lamp lights", "Luces de faroles")
X(lamp_lights_desc,
  "Street lamps, fires and neon light up the ground, walls and people near them.",
  "Las farolas, el fuego y los neones iluminan el suelo, las paredes y las personas que tienen cerca.")
X(lamp_lights_strength, "Lamp light strength", "Intensidad de las luces de faroles")
X(lamp_lights_strength_desc, "How bright the light around lamps gets.", "Cuánto iluminan las luces alrededor de los faroles.")
/* Character lighting (Display, Quality and Balanced) */
X(char_lighting, "Character lighting", "Iluminación de personajes")
X(char_lighting_desc,
  "Light from lamps, fire and neon falls on people following the shape of their bodies and faces. The HUD and text stay as they are.",
  "La luz de faroles, fuego y neones cae sobre las personas siguiendo la forma de su cuerpo y su cara. El HUD y el texto no cambian.")
/* World lighting (Display, Quality only) */
X(world_lighting, "Per-pixel scene lighting", "Iluminación del escenario por píxel")
X(world_lighting_desc,
  "Light from lamps, fire and neon falls on walls, floors and buildings per pixel, following their surfaces, on top of the original lighting.",
  "La luz de faroles, fuego y neones cae sobre paredes, suelos y edificios píxel a píxel, siguiendo sus superficies, sobre la iluminación original.")
/* Fire light (Display, Quality only) */
X(fire_light, "Fire light (barrels)", "Luz del fuego (tachos)")
X(fire_light_desc,
  "Burning barrels cast a warm, flickering light on nearby walls, floors and people. Needs per-pixel scene lighting (and character lighting for people).",
  "Los tachos encendidos dan una luz cálida y titilante a las paredes, el suelo y las personas cercanas. Necesita la iluminación del escenario por píxel (y la de personajes para las personas).")
/* Lamp shadows (Display, Quality only) */
X(lamp_shadows, "Shadows from street lamps", "Sombras de los faroles")
X(lamp_shadows_desc,
  "People near a lit street lamp or burning barrel cast a shadow on the ground pointing away from it, fading with distance, on top of the original shadow.",
  "Las personas cerca de un farol encendido o de un tacho en llamas proyectan una sombra en el suelo que se aleja de la luz y se desvanece con la distancia, sobre la sombra original.")
/* Light shafts (Display, Quality only) */
X(light_shafts, "Light shafts", "Haces de luz")
X(light_shafts_desc,
  "Street lamps hang a soft cone of light in the air below them, thicker in the rain; burning barrels glow in their smoke. Walls and people in front hide it.",
  "Los faroles proyectan un cono de luz suave en el aire, más denso con la lluvia; los tachos encendidos brillan en su humo. Las paredes y las personas delante lo tapan.")
X(light_shafts_cheap, "Light shafts: low cost", "Haces de luz: bajo costo")
X(light_shafts_cheap_desc,
  "A coarser, cheaper version of the light shafts for integrated graphics and older cards.",
  "Una versión más gruesa y barata de los haces de luz para gráficos integrados y placas antiguas.")
/* Soft particles (Display, opt-in: off in every preset) */
X(soft_particles, "Soft particles", "Partículas suaves")
X(soft_particles_desc,
  "Smoke, fire and dust fade out where they meet the ground and walls instead of ending in a hard line. No preset turns it on.",
  "El humo, el fuego y el polvo se desvanecen al tocar el suelo y las paredes en lugar de cortarse en una línea recta. Ningún preajuste lo activa.")
/* On-screen counter (Display card, off in every preset) */
X(perf_overlay_section, "On-screen counter", "Contador en pantalla")
X(perf_overlay_show, "Show counter", "Mostrar contador")
X(perf_overlay_show_desc,
  "A small counter in a corner of the game: pick below what it shows. Almost free. No preset turns it on.",
  "Un pequeño contador en una esquina del juego: elegí abajo qué muestra. Casi no cuesta nada. Ningún preajuste lo activa.")
X(perf_overlay_fps, "FPS", "FPS")
X(perf_overlay_fps_desc, "Frames per second.", "Cuadros por segundo.")
X(perf_overlay_ms, "Milliseconds per frame (16.6 ms)", "Milisegundos por cuadro (16,6 ms)")
X(perf_overlay_ms_desc, "How long each frame takes on average.", "Cuánto tarda en promedio cada cuadro.")
X(perf_overlay_cpu, "CPU usage (%)", "Uso de CPU (%)")
X(perf_overlay_cpu_desc, "The game's use of the processor; 100% means every core is busy.",
  "Cuánto usa el juego el procesador; 100% significa todos los núcleos ocupados.")
X(perf_overlay_gpu, "GPU usage (%)", "Uso de GPU (%)")
X(perf_overlay_gpu_desc, "How busy the game keeps the graphics card each frame (only the game's own work).",
  "Cuánto ocupa el juego la placa de video en cada cuadro (solo el trabajo del juego).")
X(perf_overlay_corner, "Corner", "Esquina")
X(corner_tl, "Top left", "Arriba izq.")
X(corner_tr, "Top right", "Arriba der.")
X(corner_bl, "Bottom left", "Abajo izq.")
X(corner_br, "Bottom right", "Abajo der.")
X(perf_overlay_question, "Do you want to see an FPS counter on screen?", "¿Querés ver un contador de FPS en pantalla?")
X(perf_overlay_yes, "Yes", "Sí")
X(perf_overlay_no, "No", "No")
X(perf_overlay_new, "New: on-screen performance counter", "Nuevo: contador de rendimiento en pantalla")
X(perf_overlay_new_desc,
  "Show FPS, frame time, CPU or GPU usage in a corner while you play. You can change it any time below.",
  "Mostrá los FPS, el tiempo por cuadro o el uso de CPU o GPU en una esquina mientras jugás. Lo podés cambiar cuando quieras más abajo.")
X(perf_overlay_configure, "Set up", "Configurar")
X(perf_overlay_no_thanks, "No, thanks", "No, gracias")
/* Filmic look (Display, Quality) */
X(filmic, "Cinematic look", "Look cinematográfico")
X(filmic_desc,
  "The streets look like the 1979 film: warm lights with a red glow, teal shadows and deep blacks. The HUD and text stay as they are.",
  "Las calles se ven como en la película de 1979: luces cálidas con un resplandor rojizo, sombras verdeazuladas y negros profundos. El HUD y los textos no cambian.")
/* Enhanced sky (Display, Quality) */
X(sky_hd, "Enhanced sky", "Cielo mejorado")
X(sky_hd_desc,
  "A sharper night sky in each level's own colours, with stars, drifting clouds, a moon and lit windows on the distant skyline.",
  "Un cielo nocturno más nítido con los colores de cada nivel: estrellas, nubes que se mueven, luna y ventanas encendidas en los edificios lejanos.")
/* Sharp fonts (Display, every preset) */
X(sharp_fonts, "Sharp text", "Texto nítido")
X(sharp_fonts_desc,
  "Menu, HUD, mission and subtitle letters are rebuilt at 4x from the game's own font when it starts, so they stay crisp at "
  "1080p and above. Button icons keep their look.",
  "Las letras de los menús, el HUD, las misiones y los subtítulos se reconstruyen a 4x a partir de la fuente del propio juego "
  "al arrancar, para que se vean nítidas a 1080p y más. Los iconos de los botones no cambian.")
/* Neon glow (Display, Balanced and Quality) */
X(neon_glow, "Neon glow", "Brillo de neones")
X(neon_glow_desc,
  "Lit and neon signs glow at night and through fog, and feed the bloom. Needs the emission masks folder (neon-masks).",
  "Los carteles luminosos y de neón brillan de noche y a través de la niebla, y alimentan el resplandor. Necesita la carpeta de "
  "máscaras de emisión (neon-masks).")
X(neon_glow_strength, "Neon glow strength", "Intensidad del brillo de neones")
X(neon_glow_strength_desc, "How bright the lit parts of the signs get.", "Cuánto brillan las partes iluminadas de los carteles.")
/* Sharp faces (Display, Quality) */
X(sharp_faces, "Sharp faces", "Caras nítidas")
X(sharp_faces_desc,
  "Character faces and clothes at twice the detail, made from your game's own textures the first time each one appears. A texture pack's version wins.",
  "Caras y ropa de los personajes con el doble de detalle, creadas a partir de las texturas de tu juego la primera vez que aparecen. La versión de un pack de texturas tiene prioridad.")
/* Sharp faces over the texture pack (Display, shown with Sharp faces on and a texture pack) */
X(sharp_faces_over_pack, "Sharp faces over texture pack", "Caras nítidas por encima del pack de texturas")
X(sharp_faces_over_pack_desc,
  "Character faces and clothes use Sharp faces even when your texture pack has its own version. Everything else keeps the pack.",
  "Las caras y la ropa de los personajes usan Caras nítidas aunque tu pack de texturas tenga su propia versión. Todo lo demás sigue usando el pack.")
/* Draw distance (Display, Quality) */
X(draw_distance, "Draw distance", "Distancia de dibujado")
X(draw_distance_desc,
  "How far the fog and the scenery reach, and how far away small objects (bins, crates) stay visible. Original is the game as it shipped.",
  "Hasta dónde llegan la niebla y el escenario, y a qué distancia siguen viéndose los objetos pequeños (cubos, cajas). Original es el juego tal como salió.")
X(draw_original, "Original", "Original")
X(draw_far, "Far", "Lejos")
X(draw_farther, "Farther", "Muy lejos")
X(draw_max, "Maximum", "Máxima")
X(fx_scale, "Effects and particles", "Efectos y partículas")
X(fx_scale_desc, "More blood, debris, smoke and sparks at once; blood stays through fights.",
  "Más sangre, escombros, humo y chispas a la vez; la sangre se queda durante las peleas.")
X(fx_normal, "Normal", "Normal")
X(fx_more, "More", "Más")
X(fx_max, "Maximum", "Máximo")
/* Interface size (Options) */
X(ui_scale, "Interface size", "Tamaño de la interfaz")
X(ui_scale_desc, "Makes the launcher's text and controls bigger. Auto follows your screen's scaling.",
  "Agranda los textos y controles del lanzador. Automático sigue la escala de tu pantalla.")
X(ui_scale_auto, "Auto (%s)", "Automático (%s)")
X(backup_list, "Your backups", "Tus copias")
X(hd_chars_on, "HD characters: on (%d models)", "Personajes HD: activados (%d modelos)")
X(hd_chars_empty, "HD characters: characters-hd folder is empty", "Personajes HD: carpeta characters-hd vacía")
X(warn_no_vulkan,
  "Your graphics card has no compatible Vulkan driver. The menu and the game will look black. Update your graphics card "
  "driver from the NVIDIA, AMD or Intel website.",
  "Tu tarjeta gráfica no tiene un controlador Vulkan compatible. El menú y el juego se verán en negro. Actualiza el "
  "controlador de tu tarjeta gráfica desde la página de NVIDIA, AMD o Intel.")
X(warn_renderer_none,
  "The renderer is set to None: the menu and the game will look black.",
  "El renderizador está en Ninguno: el menú y el juego se verán en negro.")
X(use_vulkan, "Use Vulkan", "Usar Vulkan")
