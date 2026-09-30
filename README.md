# Timelapse

A small native Windows timelapse recorder. Record a display, a camera, or both into H.264 or HEVC MP4 files. Use the Windows installer or the portable ZIP. No bundled browser, external encoder, or .NET runtime is required.

## Use

1. Run the Windows x64 setup program, or extract the portable ZIP and open `Timelapse.exe`. Windows 10 (version 2004 or later) or Windows 11, x64 is required. If building from source, use `dist/Timelapse.exe`.
2. Choose Desktop, Camera, Desktop + camera, Side by side, Custom collage, or Desktop + camera (2 files). Pick a display and camera where needed.
3. Choose how often to capture a frame. At the default five seconds, one hour becomes 24 seconds of video. Output plays at 30 fps, without audio.
4. In a collage, click and drag a source to move it. Drag its lower-right corner to resize it. Use Bring forward to change overlap. Changes during recording appear in subsequent frames.
5. Press Record. Pause skips recording until resumed; Finish finalizes the MP4 file or pair of files. Open folder shows the saved files. The default destination is Videos/Timelapse.

**Separate files:** Desktop + camera (2 files) saves a `-desktop.mp4` and a `-camera.mp4` with a shared capture interval, playback rate, quality, and pause/resume controls. Each file shows the complete source at the selected output size, preserving its aspect ratio. The side-by-side preview helps position the camera; it is not burned into either output. Collage editing is disabled in this mode. If a source or encoder fails, both recordings stop and the app attempts to save each file, reporting each outcome.

**System tray:** Closing the window hides Timelapse while recording continues. Reopen it from its tray icon or the Start menu. Right-click the tray icon for Show, Pause/Resume, Finish, or Exit. Exit finishes an active recording before closing; a saving failure brings the window back with the recovery information. Hidden windows stop preview processing. If Windows cannot add the tray icon, the app stays accessible in its window.

The installer keeps Timelapse in your user account's `LocalAppData\Programs\Timelapse` directory and adds Start menu and uninstall entries. Start with Windows is optional and starts the app in the tray without recording. Exit Timelapse from its tray menu before upgrading or uninstalling. Uninstalling preserves your recordings and settings.

Tab moves between controls, and Alt + O opens the save folder. In the collage preview, Space cycles between sources, arrow keys move the selected source, and Shift + arrow keys resize it. Using these keys while dragging ends the current drag and preserves the keyboard edit. Reset layout restores the preset.

On smaller work areas, scroll to reach the preview and controls. The wheel scrolls vertically, Shift + wheel scrolls horizontally, and keyboard navigation brings focused controls into view.

The quality selector offers Smaller file, Balanced, and More detail. Balanced is the default. More detail prioritizes image detail; its file size depends on the scene and can grow substantially with motion, texture, or frequent cuts.

Choose an encoding mode separately from resolution and quality:

| Encoding | Use |
| --- | --- |
| Compatible H.264 | Original software encoding settings and broad playback support. This remains the default. |
| Efficient H.264 | Tuned software encoding with a bitrate target and longer keyframe spacing. A useful starting point for long recordings. |
| Hardware H.264 | Uses an available hardware encoder to reduce CPU work, with broad H.264 playback support. |
| Hardware HEVC | Uses an available hardware HEVC encoder. Playback requires a compatible player or installed HEVC decoder. |
| Quality H.264 | Software encoding that prioritizes consistent detail. Static screens can produce small files; frequent changes can produce much larger files. |

Hardware support depends on the computer and driver. If a hardware mode is unavailable, choose Compatible H.264 or Efficient H.264. The app verifies that hardware modes actually use a hardware encoder. Encoding and quality choices are saved and remain locked during a recording.

Efficient uses variable bitrate at all three quality levels. Its bitrate is a target, not a strict file-size cap. Quality H.264 and the hardware modes use fixed quantization settings: detailed scenes, noise and frequent changes can need substantially more data. A long capture interval reduces the number of frames but can also make consecutive frames less alike. The quality labels describe a tradeoff within each mode and do not promise identical image quality or file size across different codecs. Hardware encoding can reduce CPU use while keeping a dedicated GPU awake, so lower CPU use does not establish better battery life.

Desktop capture excludes this app's window on supported Windows versions. Minimize or close the window to stop preview updates while recording continues. Camera modes activate the camera for preview and recording. The app starts in Desktop mode so opening it does not silently activate a camera.

Switching sources or devices clears the previous preview until the new selection produces a frame.

Refresh keeps the selected camera and display when the device list changes. If a selected source is unavailable, choose a replacement or refresh after it returns. Record stays disabled while a source needed by the current layout is unavailable.

Recording follows the selected display's identity and current bounds. If the display is missing or changes while a recording frame is captured, that frame is rejected and recording stops, attempting to save earlier frames.

Saving keeps ownership of the original recording through finalization and the final filename change. An existing destination is never overwritten. If the filename change fails, the status message identifies the finished video retained at its `.recording.mp4` path. If finalization itself fails, any retained partial file may be incomplete. Save folders and preference files support long local Windows paths and Unicode names.

## Build and verify

Install CMake 3.20 or later (3.21 or later for Visual Studio 2022) and Visual Studio 2019 or 2022 Build Tools with the Desktop development with C++ workload and Windows 10 SDK. Run from PowerShell:

```powershell
.\build.ps1 -Test
```

Successful builds publish `dist/Timelapse.exe`, `dist/Timelapse-portable.zip`, and `dist/SHA256SUMS.txt`. These files are prepared before replacing the previous distribution. If publication fails, the script attempts to restore the previous files. If recovery cannot finish, the error identifies the retained staging folder.

The build statically links the C++ runtime. Automated tests exercise compositor geometry and pixels, actual MP4 encoding and decoding, and recording lifecycle behavior through Windows Media Foundation. The desktop integration test requires an unlocked interactive Windows session with desktop capture access.

If PowerShell scripts are disabled, run the equivalent commands directly (use `Visual Studio 17 2022` for VS 2022):

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

For checks that use only synthetic capture sources, add `-E "^engine_tests$"` to the CTest command. This excludes the desktop recording integration test.

The executable is then in `build/release/Timelapse.exe`. Developers can launch it with `--inspect-ui` to allow window screenshots during visual QA; normal launches exclude the app from desktop capture.

## Current limits

- One display and one camera at a time; 720p or 1080p output, preserving source aspect ratios. Camera input is capped at 720p to limit processing; 1080p preserves additional desktop detail.
- The desktop must remain unlocked and awake. Protected content may appear black. A removed display or unavailable camera stops recording and attempts to save captured frames.
- MP4 is finalized when Finish is pressed or Exit is selected from the tray. Closing the window keeps recording. A power loss or forced termination can leave an unplayable `.recording.mp4` file; crash recovery is not implemented yet.
- Windows N requires the Media Feature Pack. The app and installer are currently unsigned.
- Camera compatibility and performance vary by device; automated media tests use generated frames and do not establish physical-camera compatibility.
- Display identity uses a Windows monitor interface when available, with a GDI display-name fallback. The fallback cannot distinguish a replacement using the same name; changes that disappear and return entirely within one capture can escape detection.

The app retains the latest preview and camera frame, and writes samples incrementally. Preview runs at two frames per second when idle and one while recording; minimized windows do not generate preview frames. There is no growing in-memory recording buffer. The app requests that Windows stay awake while recording or saving. It releases the request while paused and after saving finishes. Manual locking is still respected.

Camera access runs in a private helper process launched from the same executable. A stuck camera driver can be stopped without trapping the app in shutdown. The helper exits with its parent and uses only local shared memory; there are no network services.

The recording worker sleeps until a capture, preview, clock update or command is due. Hidden idle and paused sessions have no periodic worker tick. The camera helper converts and copies pixels only when requested; its camera reader continues receiving current samples. This reduces unnecessary work between captures without making an old frame appear fresh.

Media implementation references: [Microsoft's sink writer tutorial](https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video) and [asynchronous source reader](https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode).

## Reproduce encoding measurements

In the v0.2.0 encoding validation, Efficient H.264 reduced file size by 7–60% and encoding CPU time by 13–61% versus Compatible H.264 in the same build. Both used Balanced quality. These were four synthetic 180-frame, 1920×1080 clips at 30 fps on an AMD Ryzen 7 5800H; CPU times are the mean of two alternating runs:

| Scene | Compatible / Efficient file size (MB) | Compatible / Efficient encoding CPU (seconds) |
| --- | --- | --- |
| Mostly static desktop text | 0.878 / 0.349 | 3.70 / 3.23 |
| Scrolling and cuts | 3.105 / 2.875 | 4.97 / 3.73 |
| Textured motion and noise | 6.385 / 5.110 | 23.01 / 9.05 |
| Large changes every frame | 7.042 / 5.213 | 30.55 / 15.74 |

MB means 1,000,000 bytes. Encoding CPU is accumulated process CPU time, including conversion and finalization, and excludes capture and input generation. It is not elapsed recording time or a battery measurement. Repeated runs produced identical compressed packets and timestamps. Results depend on scene, hardware and Windows encoder implementation; a bitrate target is not a strict cap.

Independent decoding found a modest fidelity tradeoff: Efficient's pooled luma PSNR was 0.86 dB lower on the static scene and 0.31 dB lower on frequent cuts, but 1.08 dB higher on scrolling and 0.54 dB higher on motion. Motion's worst-frame block SSIM improved from 0.896 to 0.919; frequent cuts stayed close at 0.731 versus 0.730. All frames, timestamps, durations, dimensions and color tags passed verification. These metrics describe the test clips, not perceptual quality for every source.

Optional tools generate synthetic desktop text, scrolling/cuts, camera-like motion/noise, and large changes between captures. They do not capture a display or camera. Build them separately:

```powershell
cmake -S . -B build-benchmark -G "Visual Studio 16 2019" -A x64 -DTIMELAPSE_BUILD_BENCHMARKS=ON
cmake --build build-benchmark --config Release --target encoding_benchmark encoding_quality_verifier
.\build-benchmark\Release\encoding_quality_verifier.exe --self-test
.\build-benchmark\Release\encoding_benchmark.exe efficient balanced 0 1920 1080 180 screen.mp4
.\build-benchmark\Release\encoding_quality_verifier.exe screen.mp4 0 1920 1080 180
```

Use a new output filename for every run. Modes are `compatible`, `efficient`, `hardware-h264`, `hardware-hevc`, and `quality-h264`; qualities are `compact`, `balanced`, and `detail`. Scenes `0`, `1`, and `2` exercise a mostly static screen, scrolling/cuts, and textured motion/noise. Scene `3` stresses large changes and frequent cuts between captures. A 180-frame clip crosses the new modes' five-second keyframe boundary.

The benchmark pre-renders its input outside the encoding measurement. At 180 frames of 1080p this requires about 1.4 GiB of temporary benchmark memory; the recorder itself streams frames. CSV output reports total encoding CPU time, wall time, setup/submission/finalization CPU time, and sampled additional process-private memory. GPU memory and energy are not included. Repeat modes in alternating order without other benchmark or build jobs running.

The independent verifier checks frame count, every timestamp, duration, luma/chroma fidelity, block SSIM, and small-text edges. PSNR is pooled across frames; whole-image averages can conceal local damage, so inspect text and the worst frames too. Color differences from NV12's 4:2:0 subsampling are included in the RGB text metric. These are synthetic measurements, not a guarantee for every recording.

If Windows has no decoder for a tested codec, use the independent FFmpeg verifier. Install `ffmpeg` and `ffprobe` on PATH, then run:

```powershell
.\tools\verify-encoding-quality.ps1 -InputVideo screen.mp4 -Scene 0 -Width 1920 -Height 1080 -Frames 180 -Verifier .\build-benchmark\Release\encoding_quality_verifier.exe
```

This also checks BT.709 limited-range color metadata. FFmpeg is used only by the optional verification script and is not required by Timelapse. The script writes metadata and quality reports next to the benchmark video and removes its temporary decoded frames.

## Repository contents

- `src/`: the native Windows app, capture worker, compositor, and MP4 encoder.
- `tests/`: media roundtrips, recording recovery, file collision protection, camera helper isolation, and a 600-frame 1080p resource check.
- `tools/verify-encoding-quality.ps1`: optional independent FFmpeg decoding and quality checks for synthetic benchmark videos.
- `CMakeLists.txt` and `build.ps1`: build and test the app.
- `package.ps1`: create a portable release ZIP and a source ZIP from a Release build.
- `installer/`: Inno Setup source and compiler wrapper for the per-user installer.

After building and testing, run `./package.ps1` to create both archives in `packages/`. The release ZIP is intended for GitHub Releases. Extract the source ZIP into an empty folder, then run the build commands from that folder; build outputs, test recordings, and local settings are excluded.

To also create `Timelapse-v0.3.0-windows-x64-setup.exe`, supply an installed or portable Inno Setup 7 compiler:

```powershell
.\package.ps1 -InstallerCompiler 'C:\Path\To\Inno Setup 7\ISCC.exe'
```

The setup EXE, both ZIPs, and their checksums are published together from frozen inputs. The compiler is a build dependency and is not bundled with the app. To exercise installation, upgrades, opt-in startup, running-app protection and uninstall in an isolated directory without creating a real installed-app registration or Start menu entries:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\installer_tests.ps1 -Compiler 'C:\Path\To\Inno Setup 7\ISCC.exe' -Executable .\dist\Timelapse.exe -WorkDirectory .\.tmp\installer-test
```

Use a new work directory and exit other Timelapse instances before this installer test. It uses the production installer source with only destination and registration overrides; it does not launch the recorder.

Archives and checksums are prepared before replacing existing packages. If publication fails, the script attempts to restore the previous files. If recovery cannot finish, the error names the staging directory containing the backups.

Run the distribution and packaging regression checks with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\build_tests.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\package_tests.ps1
```

They use temporary projects and dummy executable bytes. The build-script checks substitute the build and test commands, so they do not compile or launch the app.
