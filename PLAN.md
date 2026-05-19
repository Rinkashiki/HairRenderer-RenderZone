# Plan

Forward-looking work for this project. Completed features are tracked in git history; reference docs live in `CLAUDE.md` and `ANIMATION.md`.

---

## Open

### SLViewer — Windows clean-machine deploy validation

Local Windows smoke test now passes (see Done below). **Still not validated on a clean Windows host** (no Vulkan SDK, no VS), and there is a known blocker for that step:

- `CMakeLists.txt:247-264` only looks for `vulkan-1.dll` at `%VULKAN_SDK%\Bin\vulkan-1.dll`. The LunarG SDK 1.4.x **no longer ships a redistributable `vulkan-1.dll` in the SDK tree** — the installer puts the loader in `C:\Windows\System32\vulkan-1.dll` instead. Result: configure emits the "vulkan-1.dll not found" warning and `cmake --install` produces a folder **without** the loader. It runs fine locally (System32 loader is found) but will fail on a machine with no Vulkan runtime.
- Fix candidates (not yet applied — needs decision): extend the search to `%VULKAN_SDK%\runtime\x64`, then `%SystemRoot%\System32\vulkan-1.dll`; optionally escalate the WIN32 "not found" case to a `FATAL_ERROR` so a broken distributable can't be silently produced.

Remaining steps once the bundling is fixed:
1. Reconfigure/rebuild/install so `vulkan-1.dll` lands in `C:\SLViewer-windows\`.
2. Copy the folder to a clean Windows host (no Vulkan SDK, no VS, no IDE).
3. Run `SLViewer.exe test_anim.json` and confirm `test_anim.mp4` is produced, process exits 0, temp dir cleaned.

---

## Done

### SLViewer — Windows ffmpeg export fix + local smoke test (2026-05-19)

`SLViewer.exe` rendered all frames but failed at the encode step with *"El nombre de archivo... no son correctos"* (Windows ERROR_INVALID_NAME) and `ffmpeg exited with code 1`.

**Issues found:**
1. `resource_paths.cpp` `get_ffmpeg_path()` looked for a bundled `ffmpeg` with no extension; the actual file is `ffmpeg.exe`, so `std::filesystem::exists()` failed and it fell back to bare `"ffmpeg"` (not on PATH).
2. `video_encoder.cpp` built a Windows command starting with a quote and containing several quoted tokens. `std::system()` runs `cmd.exe /c <str>`; cmd strips the outermost quote pair and corrupted the executable path.

**Solutions implemented:**
1. `get_ffmpeg_path()` now picks `ffmpeg.exe` on `_WIN32` (and falls back to that name for PATH lookup).
2. `VideoEncoder::encode()` wraps the entire Windows command in one extra pair of double quotes so the inner quoting survives `cmd /c`.

**Verified:** Release rebuild + `cmake --install` → `C:\SLViewer-windows`; `SLViewer.exe test_anim.json --output test_anim.mp4` produced a 232 KiB / 120-frame MP4, ffmpeg exit 0, frames sourced from `%TEMP%\slviewer_<pid>\`.

---

## Upcoming

_Add new initiatives below as they come up._
