# Plan

Forward-looking work for this project. Completed features are tracked in git history; reference docs live in `CLAUDE.md` and `ANIMATION.md`.

---

## Open

### SLViewer — Windows end-to-end smoke test

Linux distributable is verified. Windows path is built and packaged (MSVC `/MT`, `vulkan-1.dll` copied from `%VULKAN_SDK%\Bin`, BtbN static GPL `ffmpeg.exe` fetched at configure time) but has not been validated on a clean Windows machine.

Steps:
1. `cmake -G "Visual Studio 17 2022" .. && cmake --build . --config Release --target SLViewer`
2. `cmake --install . --config Release --prefix C:\SLViewer-windows`
3. Copy the folder to a clean Windows host (no Vulkan SDK, no VS, no IDE).
4. Drop `test_anim.json` next to `SLViewer.exe` and run `SLViewer.exe test_anim.json`.
5. Expected: `test_anim.mp4` produced, ~4 s @ 30 fps, process exits 0, temp dir cleaned.

---

## Upcoming

_Add new initiatives below as they come up._
