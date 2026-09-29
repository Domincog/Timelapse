# Timelapse

A small native Windows timelapse recorder. Record a display, a camera, or both into an H.264 MP4. No installer, bundled browser, external encoder, or .NET runtime is required.

## Use

1. Extract the portable release ZIP, then open `Timelapse.exe` on Windows 10 (version 2004 or later) or Windows 11, x64. If building from source, use `dist/Timelapse.exe`.
2. Choose Desktop, Camera, Desktop + camera, Side by side, or Custom collage. Pick a display and camera where needed.
3. Choose how often to capture a frame. At the default five seconds, one hour becomes 24 seconds of video. Output plays at 30 fps, without audio.
4. In a collage, click and drag a source to move it. Drag its lower-right corner to resize it. Use Bring forward to change overlap. Changes during recording appear in subsequent frames.
5. Press Record. Pause skips recording until resumed; Finish finalizes the MP4. Open folder shows the saved files. The default destination is Videos/Timelapse.

Tab moves between controls. In the collage preview, Space cycles between sources, arrow keys move the selected source, and Shift + arrow keys resize it. Reset layout restores the preset.

Desktop capture excludes this app's window on supported Windows versions. Minimize the app to stop preview updates while recording continues. Camera modes activate the camera for preview and recording. The app starts in Desktop mode so opening it does not silently activate a camera.

## Build and verify

Install CMake and Visual Studio 2019 or 2022 Build Tools with the Desktop development with C++ workload and Windows 10 SDK. Run from PowerShell:

```powershell
.\build.ps1 -Test
```

The portable executable is copied to `dist/Timelapse.exe`. The build statically links the C++ runtime. Automated tests exercise compositor geometry and pixels, actual MP4 encoding and decoding, and recording lifecycle behavior through Windows Media Foundation. The desktop integration test requires an unlocked interactive Windows session with desktop capture access.

If PowerShell scripts are disabled, run the equivalent commands directly (use `Visual Studio 17 2022` for VS 2022):

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The executable is then in `build/release/Timelapse.exe`. Developers can launch it with `--inspect-ui` to allow window screenshots during visual QA; normal launches exclude the app from desktop capture.

## Current limits

- One display and one camera at a time; 720p or 1080p output, preserving source aspect ratios. Camera input is capped at 720p to limit processing; 1080p preserves additional desktop detail.
- The desktop must remain unlocked and awake. Protected content may appear black. A removed display or unavailable camera stops recording and attempts to save captured frames.
- MP4 is finalized when Finish is pressed or the app closes normally. A power loss or forced termination can leave an unplayable `.recording.mp4` file; crash recovery is not implemented yet.
- Windows N requires the Media Feature Pack. The portable executable is currently unsigned.
- Camera compatibility and performance vary by device; automated media tests use generated frames and do not establish physical-camera compatibility.

The app retains the latest preview and camera frame, and writes samples incrementally. Preview runs at two frames per second when idle and one while recording; minimized windows do not generate preview frames. There is no growing in-memory recording buffer. While recording, the app requests that Windows stay awake; pausing or finishing releases this request. Manual locking is still respected.

Camera access runs in a private helper process launched from the same executable. A stuck camera driver can be stopped without trapping the app in shutdown. The helper exits with its parent and uses only local shared memory; there are no network services.

Media implementation references: [Microsoft's sink writer tutorial](https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video) and [asynchronous source reader](https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode).

## Repository contents

- `src/`: the native Windows app, capture worker, compositor, and MP4 encoder.
- `tests/`: media roundtrips, recording recovery, file collision protection, camera helper isolation, and a 600-frame 1080p resource check.
- `CMakeLists.txt` and `build.ps1`: build and test the app.
- `package.ps1`: create a portable release ZIP and a source ZIP from a Release build.

After building and testing, run `./package.ps1` to create both archives in `packages/`. The release ZIP is intended for GitHub Releases. Extract the source ZIP into the repository root; build files, test recordings, and local settings are excluded.
