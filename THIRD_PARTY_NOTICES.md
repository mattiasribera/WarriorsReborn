# Third-party notices

WarriorsReborn is free software under the GNU General Public License version 3 or later
(`LICENSE`). It includes or is built with the third-party software below. The release zip carries
each license text in its `licenses/` folder (the file names are in the last column).

| Software | Where it is used | License | License text in the zip |
| --- | --- | --- | --- |
| [SDL](https://www.libsdl.org/) 2.32.10 | `SDL2.dll`: window, sound, controllers | zlib | `SDL2-LICENSE.txt` |
| [Steam Audio](https://github.com/ValveSoftware/steam-audio) 4.8.1 (Valve) | `phonon.dll`, built from source with the port's patches (`runtime/third_party/steam-audio`): headphone (HRTF) sound, occlusion through level geometry, location-based reverb, sound around corners | Apache-2.0; the code and data inside the library have their own notices | `steam-audio-LICENSE.md`, `steam-audio-THIRDPARTY.md`, `steam-audio-TRADEMARK_RIGHTS.md` |
| [Basis Universal](https://github.com/BinomialLLC/basis_universal) transcoder (Binomial) | compiled into `xbrun-core.dll`: KTX2 texture packs | Apache-2.0 | `basisu-LICENSE.txt`, `basisu-NOTICE.txt` |
| [zstd](https://github.com/facebook/zstd) decoder (Meta), as bundled by Basis Universal | compiled into `xbrun-core.dll` | BSD-3-Clause | `zstd-LICENSE.txt` |
| [AMD FidelityFX](https://gpuopen.com/fidelityfx/) Super Resolution 1 (EASU, RCAS) and SDK 1.1.4 SSSR, ported to GLSL | shaders compiled into `xbrun-core.dll`: `--fsr`, the TAA sharpening, `--reflections sssr` | MIT | `FidelityFX-LICENSE.txt` |
| [XeGTAO](https://github.com/GameTechDev/XeGTAO) (Intel), ported to GLSL | shaders compiled into `xbrun-core.dll`: `--ssao-mode gtao` | MIT | `XeGTAO-LICENSE.txt` |
| [stb_image](https://github.com/nothings/stb) 2.30, stb_image_write 1.16 | compiled into `xbrun-core.dll` | MIT or public domain | `stb-LICENSE.txt` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 1.4.363 (Khronos) | compiled into `xbrun-core.dll` | Apache-2.0 or MIT | `Vulkan-Headers-LICENSE.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.5 | compiled into `warriors-launcher.exe` | MIT | `imgui-LICENSE.txt` |
| [Inter](https://rsms.me/inter/) 4.1 font | embedded in `warriors-launcher.exe` | SIL Open Font License 1.1 | `Inter-LICENSE.txt` |
| LLVM libc++, libc++abi, libunwind (from zig 0.17.0) | linked into `xbrun-core.dll` and `warriors-launcher.exe` | Apache-2.0 with LLVM exception | `libcxx-LICENSE.txt`, `libcxxabi-LICENSE.txt`, `libunwind-LICENSE.txt` |
| mingw-w64 runtime support code (from zig 0.17.0) | all Windows programs | ZPL 2.1 (some parts public domain or BSD) | `mingw-w64-COPYING.txt` |

In the source repository the license texts are next to the code: `runtime/third_party/*/`,
`launcher/third_party/*/`, and `runtime/third_party/notices/` for the ported shaders (whose own
headers also carry the notices).

Optional tools in `tools/` that are not part of the release zip may download open models on first
use (for example DeepBump, GPL-3.0, in `tools/relief_from_albedo_ai.py`); each tool's header names
what it fetches and its license.

Not included, provided by your system: the Microsoft Universal C Runtime (part of Windows 10/11)
and the Vulkan loader (`vulkan-1.dll`) with the Vulkan driver, which come with Windows and your
graphics driver.

The Warriors and all related names and marks are trademarks of their respective owners; "Xbox" is
a trademark of Microsoft; "Steam" is a trademark of Valve. This project is not affiliated with,
endorsed by or sponsored by Rockstar Games, Take-Two Interactive, Paramount, Microsoft, Valve, AMD
or Intel, and contains none of their game files.
