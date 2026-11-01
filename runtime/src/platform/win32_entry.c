/* xbrun-core.dll entry called by the TheWarriors.exe loader (win32_loader.c). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

/* argv from the wide command line in the ANSI code page (the CRT file APIs take ANSI paths). */
int xbrun_main(int argc, char **argv);

__declspec(dllexport) int xbrun_dll_main(void) {
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return 1;
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_ACP, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc((size_t)n);
        WideCharToMultiByte(CP_ACP, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    LocalFree(wargv);
    /* Load the host libraries while the process image is still the loader's (their DllMain
     * runs now); plat_dlopen later only takes another reference. Missing ones are fine. */
    LoadLibraryA("SDL2.dll");
    LoadLibraryA("vulkan-1.dll");
    exit(xbrun_main(argc, argv));   /* never return: the loader's code is under the XBE image now */
}
