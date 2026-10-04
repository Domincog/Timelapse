# Timelapse

A small native Windows timelapse recorder. Record a display, a camera, or both into H.264, HEVC or AV1 MP4 files. Use the Windows installer or the portable ZIP. No bundled browser, external encoder, or .NET runtime is required.

## Use

1. Run the Windows x64 setup program, or extract the portable ZIP and open `Timelapse.exe`. Windows 10 (version 2004 or later) or Windows 11, x64 is required. If building from source, use `dist/Timelapse.exe`.
2. Choose Desktop, Camera, Desktop + camera, Side by side, Custom collage, or Desktop + camera (2 files). Pick a display and camera where needed. A collage can also save separate desktop and camera files at the same time, and the source can change while recording.
3. Choose how often to capture a frame. At the default five seconds and 30 fps playback, one hour becomes 24 seconds of video. Output has no audio. Change playback FPS through Advanced → Playback & shortcuts.
4. In a collage, click and drag a source to move it. Drag its lower-right corner to resize it. Use Bring forward to change overlap. Changes during recording appear in subsequent frames.
5. Press Record. Pause skips recording until resumed; Finish finalizes the MP4 file or set of files. Open folder shows the saved files. The default destination is Videos/Timelapse.

**Change speed while recording:** Capture every remains available while recording or paused, including Custom intervals. For example, switch from 5 seconds to 60 seconds before leaving, then back to 5 seconds when you return. Future captures use the new interval in the same video; saved frames and playback FPS stay unchanged. The next capture is scheduled from the last saved sample, so shortening a long interval takes effect promptly without a catch-up burst. Capture or encoding already underway finishes first. Both files in two-file mode share the change, and a paused edit applies when you resume. The latest accepted interval is remembered on normal exit.

With Night camera, an accepted blend finishes at its full duration before later windows use the new interval. Auto blend duration stays within the new base interval. Live intervals must be at least 1 second and at least the fixed Night blend duration, if selected; invalid changes leave recording at its previous interval. Time compression retains its selected multiplier, ramp phase and observation history, applying them to the new base interval. Capture every stays locked during the start countdown, preparation and final saving.

Open folder uses the save destination selected when clicked and creates it if needed. Slow folder operations leave the recording controls responsive. The button shows Opening... while an Open folder or Show files request is outstanding; another request can start after it finishes. Hiding or exiting cancels a request that has not yet been sent to Explorer. A request already sent may still finish, and directory creation already in progress may complete. Folder-opening errors do not replace recording or recovery details.

The preview, coloured recording state, Record/Pause/Finish controls and status message occupy the left side. Input, output and save settings live in the right panel. Expanding Advanced scrolls that panel while the preview and recording controls stay visible. Hover over the save path or a status/detail label to read its full text. Display and Camera choices appear only for sources in use; collage tools appear only for editable collages.

Use the mouse wheel over settings to scroll the panel, or Tab to bring a control into view. Shift + wheel scrolls horizontally when needed. On very small work areas or at high display scaling, the window provides scrolling in both directions so every control remains reachable.

When a camera source is selected and the camera list cannot be loaded, choose Details beside the status message, or hover over it, for the Windows error and a retry hint. The reason also appears in the status line when no current error, saved result or settings warning takes priority.

**Status details:** Save results, errors and settings warnings offer a compact Details button (Alt + I). It opens the full message in a selectable, read-only view: use Ctrl + A and Ctrl + C to copy the text, including long recovery paths. The view is a snapshot from when you opened it; recording continues, and reopening shows the current details. Recording outcomes appear before any additional settings or source diagnostic. Close or Escape returns to the main window. A failed recording's recovery details remain available after Refresh or source changes; starting a new recording replaces the previous outcome.

When the report has finished files, Show files (Alt + F within Details) asks File Explorer to select them. This uses the opened report's paths, even if you later change the save folder or another split part finishes. It supports one video, a pair or a set of three files, and finished videos retained at their `.recording.mp4` names after a rename failure. Unfinished partial files are described in the report without a Show files action for those paths. Folder lookup runs only when requested. Closing Details does not wait for a slow folder: it cancels requests still locating their files, while an Explorer request already underway may finish. A lookup error leaves the original report available to copy.

**Custom values:** Capture every and Video size keep their presets and offer Custom. Capture intervals can be from 0.1 seconds to 24 hours, in exact milliseconds. Video dimensions can be even numbers from 48 to 4096 pixels per side, with at most 8,847,360 pixels (4096 x 2160); portrait and square outputs are supported. The preview follows the chosen aspect ratio. Installed encoders may reject some sizes; try a smaller size or another encoding mode if this happens. Large outputs need more memory and encoding time. The actual capture rate can be lower than requested when processing is slow; missed slots are skipped without a catch-up burst. Custom dialogs keep the previous value until you accept valid input.

**Source sizes:** Video size also offers the selected screen's resolution and, once verified camera frames are available, the camera's current input size. Use screen or Use camera input copies that size into the fixed output setting. Fit screen indicates a size reduced or rounded to meet the even-dimension and total-pixel limits; it preserves aspect within pixel rounding. These choices do not make output size follow later device changes. Camera suggestions show the input actually received, not the camera's advertised maximum. No extra camera activation is needed to populate the list.

Camera input stays within 1280 x 720 when the output fits inside that box. Larger outputs request up to 1920 x 1080 from the camera's supported modes, including for Night and separate files. A camera may supply a smaller input; larger saved dimensions cannot add detail beyond that input. Switching between these two output ranges while idle restarts the camera, so its size suggestion is temporarily unavailable until a fresh frame arrives. Recording keeps its original input limit. Higher-resolution capture uses more memory and processing, even with a long capture interval; the 720p default keeps its existing input budget.

**Separate files:** Desktop + camera (2 files) saves a `-desktop.mp4` and a `-camera.mp4` with a shared capture interval, playback rate, quality, and pause/resume controls. Each file shows the complete source at the selected output size, preserving its aspect ratio. The side-by-side preview helps position the camera; it is not burned into either output. Collage editing is disabled in this mode. If a source or encoder fails, both recordings stop and the app attempts to save each file, reporting each outcome.

**Several videos at once:** With Desktop + camera, Side by side or Custom collage, Also save separate files (below Camera) adds a full-frame Desktop and/or Camera video next to the combined one. For example, Desktop + camera with both boxes checked saves the overlay video plus separate desktop and camera timelapses. Each source is captured once per frame and encoded once per file, so every file shares the capture interval, playback FPS, size, quality, encoding mode, pause/resume, Stop after, file splits, time compression, Night blending and watermark, and their frames line up exactly. The combined video keeps the plain session name; the others end in `-desktop.mp4` and `-camera.mp4`. Collage edits during recording change only the combined video. Each extra file adds an encoder's processing time and disk space, and with AV1 about 135 MiB of memory at 1080p. The choices are remembered and fixed while recording. They are not offered for Desktop, Camera or Desktop + camera (2 files), which already save each source once. If any file fails, recording stops and the app attempts to save every file, reporting each outcome.

**Change the source while recording:** Source stays available while recording or paused, so one video can switch between Desktop, Camera, Desktop + camera, Side by side and Custom collage. For example, start with the desktop alone and add the camera overlay later, or cut between the desktop and the camera. A change applies from the next captured frame; output size, devices, saved files and other recording settings stay as they were at Record. Choose the display and camera before recording: a source that was not selected then cannot be added, and Desktop + camera (2 files) can be chosen only before recording. A source added during recording is optional. If it cannot start (for example, another app is using the camera), recording continues without it and the status explains why; when nothing else is in the layout, frames are skipped until you switch back. Switching away and back retries it. The tray icon menu offers the same Source choices while the window is hidden. Sources selected at Record keep the usual behavior: if one fails, recording stops and saves. Switching to a layout without the camera turns the camera off; switching back restarts it, which can take a few seconds. Night blending applies only if the camera was in the layout when recording started.

**Desktop cursor:** Advanced includes Show desktop cursor (Alt + K), enabled by default. Turn it off to omit the Windows mouse cursor from desktop previews and saved desktop content, including collages and the desktop file in two-file mode. The choice is remembered and stays fixed during recording. Camera-only mode retains the choice for later desktop use. Quiet-scene checks always omit the cursor. This controls Timelapse's cursor overlay; it cannot remove pointers already drawn into an application's pixels.

**System tray:** Closing the window hides Timelapse while recording continues. Reopen it from its tray icon or the Start menu. Right-click the tray icon for Show, Pause/Resume, Finish, Set status, Clear status, or Exit. While recording or paused, its Source submenu switches what the recording shows without reopening the window. Exit finishes an active recording before closing; a saving failure brings the window back with the recovery information. Hidden windows stop preview processing. If Windows cannot add the tray icon, the app stays accessible in its window.

While recording, paused or saving, the menu also shows active recording time and total accumulated video time at the recording's selected FPS. Before the first second of video, it shows the frame count instead. Totals include all split parts; files saved together share one timeline and are counted once. These are accepted-frame totals, not a guarantee that the current file has finished saving. The heading is a snapshot from when you opened the menu; reopen it to refresh. Viewing it does not restore the preview or add background polling.

A recording failure received while hidden opens the window with its details. Opening the tray menu does not dismiss that notice. A fresh failure is shown before Hide or Exit can dismiss it; after it has been shown, those controls work normally.

Hiding to the tray cancels open settings drafts. Custom-value dialogs leave keyboard focus out of the hidden window; ordinary Cancel returns focus to the setting you were editing.

**Delay next recording:** Advanced offers a self-timer: None, 5 seconds, 10 seconds, 30 seconds, 1 minute, or 5 minutes. Press Record to begin the countdown, then arrange the scene or hide the window. Preparation starts after the delay; camera startup and an initial Night blend can add more time before the first saved frame. Ordinary visible preview continues during the countdown. Stop after, file splits, time-compression ranges and the elapsed-time watermark count active recording time, excluding the countdown and preparation.

Cancel start cancels a waiting or preparing recording. Closing the window keeps the armed countdown running in the tray; Exit cancels it. The selected delay is remembered, but restarting the app never restores an armed countdown. Windows sleep or hibernation cancels a delayed start that has not yet admitted its first frame; starting again requires an explicit Record. The app requests that the system stay awake during the countdown without keeping the display on. This is a short self-timer, not a calendar or recurring schedule.

**Advanced options:** Expand Advanced (Alt + A) for the encoder selector and optional Stop after limit. Choose a preset or Custom for a positive active duration in seconds, minutes, hours, or days. Custom limits must equal a whole number of seconds, up to 2,147,483,647 seconds. Initial startup and paused time do not count. Reported elapsed time includes capture or encoding work up to the point recording stops; final saving is excluded. Timelapse automatically finishes and saves the video or pair of videos at the limit, including while hidden in the tray. Finishing may wait for a capture or encoding operation already in progress and for the files to save. The default is Never. The selected limit is remembered. The collapsed Advanced label shows active options, or a warning when Night blend or MP4 recovery settings need correction; Advanced recording settings remain locked while recording. Finishing a hidden recording also releases the camera.

**Playback & shortcuts:** A single button in Advanced opens a compact dialog for playback FPS and optional global Pause / resume, Stop and save, and Set status shortcuts. FPS accepts whole numbers from 1 to 120 and defaults to 30. It sets how quickly saved frames play; Capture every still sets how often a new frame is taken. Higher FPS makes the same captured frames play faster and produces a shorter video. It does not create extra frames. The chosen rate applies to both separate files and every split part; video-time statistics and watermark target speed use that rate.

The shortcuts start disabled. Click a shortcut field and press a combination containing Ctrl or Alt, or use Clear to disable it. Add Ctrl to Alt+letter combinations so the interface's existing access keys stay available. The shortcuts must differ. Set status opens the Status window from anywhere, including while Timelapse is hidden in the tray. Reserved or unavailable combinations show a message when you press OK, keeping your previous settings. Pause / resume operates an existing recording; Stop and save finishes it using the ordinary save flow, or cancels a delayed/preparing start. Shortcuts work while Timelapse is hidden in the tray and never start a new recording. They are suspended while a Timelapse dialog or menu is open. FPS and shortcuts are remembered and can be changed while recording is stopped.

**Split files every:** Advanced can periodically save completed parts while the recording continues. Never is the default; choose 15 minutes, 1 hour, 6 hours, 24 hours, or a Custom duration in whole seconds. This measures active recording time, not video playback time: initial preparation and pauses are excluded, while saving and opening the next part count. Every part remains a separate MP4 at the selected playback FPS. Very short parts require more encoder restarts, increase overhead, and may cause missed capture slots.

Parts share a session name with numbered `-part-000001` suffixes. In two-file mode or with Also save separate files, each part has the matching set of files. The app saves a nonempty part at its time boundary even when the next frame is not due, and opens the next part only when a frame is ready. Empty time windows create no files. For example, with ten-minute splits and frames captured at 0 and 35 minutes, the first part saves around minute 10 and the second around minute 40. Capture or encoding already in progress can delay saving.

Splitting keeps the overall Stop after deadline, capture schedule, time-compression observations and full Night blend windows. Frame and elapsed-time statistics remain session totals; the parts-saved count treats each set of files as one part. Status identifies the latest output set; earlier parts stay in the same folder. A save failure stops the session and reports the affected files. Already saved parts remain available; splitting does not guarantee recovery of the currently open part after a crash or power loss.

**Low disk space:** Advanced also contains Stop on low disk space, enabled by default. Before opening a recording and before each captured frame is admitted, Timelapse checks the space available to your account in the save folder. It stops and attempts to save when 64 MiB or less remains per output file (128 MiB for two files, 192 MiB for three). If Windows cannot report available space, recording is refused or stopped with a diagnostic. The option is remembered and locked during a recording; turn it off for a folder that cannot provide space information. Checks do not run for preview, idle time, or paused recordings.

This is a best-effort headroom check, not reserved disk space: another program, encoder buffering, large video indexes, or drive failure can still prevent saving. A slow network-folder query can delay recording or Finish. The original stop reason remains visible if saving or renaming also fails.

**MP4 recovery mode:** Advanced offers an optional H.264 recording format that writes small completed sections as recording proceeds. After a forced app termination, completed sections in the retained `.recording.mp4` file may play in a compatible player without first pressing Finish. Recent frames and files interrupted before initialization may still be lost. This does not guarantee recovery after power loss, drive failure or every type of interruption, and it does not repair existing recordings.

Recovery mode is off by default. It adds container bytes and processing work without changing the selected H.264 compression settings. Some players and editors may not accept fragmented MP4; use ordinary MP4 for the widest compatibility. Recovery works with any available H.264 encoder and with single, separate or companion files, Night blending and time compression. Hardware HEVC and AV1 require recovery mode to be off; an incompatible selection stays visible and blocks Record until corrected. Finish still saves the file normally. The option is remembered and locked during a recording.

Accepted settings are saved before a new recording starts as well as on normal exit. A preferences-write failure does not prevent recording and leaves the previous saved settings intact.

**Watermark:** Advanced offers an optional text overlay, off by default. Show active elapsed time, recorded local date/time, the total target speed, or time and speed together. Choose a corner or set custom horizontal and vertical percentages, and select Small, Medium or Large text. The text has no background box: it is drawn in Segoe UI with a thin dark outline and soft shadow, so it reads over light and dark content, and its lines align toward the nearer side. The preview shows the placement within the chosen video size; the text stays within the frame. Settings are remembered and remain fixed during a recording, with the same values and placement on every file.

Elapsed time excludes the self-timer, initial preparation and pauses and continues across automatic file splits. Recorded local date/time is sampled when the prepared frame is labeled for saving; it is not the camera's exposure time and can jump when the system clock changes. Target speed describes planned capture spacing at the selected playback FPS: at 30 fps, Capture every 5 seconds gives Target 150x, and an additional 4x time-compression interval gives Target 600x. At 60 fps those targets double. Processing delays and skipped capture slots can make the actual speed different. The first frame uses the base interval. Idle preview uses illustrative elapsed/speed values; a paused recording keeps its last saved label.

The watermark is added after source processing, so its changing text does not trigger image-change or person checks or alter Night blending. Its native text tile is cached; no font package, image model or extra capture is needed. The live status line below uses the same drawing. Very small video sizes can be too narrow for the selected text, in which case choose a larger output, fewer fields or smaller text. Adding changing text can increase encoded file size, especially on an otherwise static scene.

**Status in the video:** Show what you're doing in a corner of the video, like a game's event feed. In the STATUS section of the settings panel choose Set status... (Alt + U), or use the tray menu or an optional global shortcut, and type a short line such as "Shower", "Work on Essay" or "First task done!!" (up to 60 characters). Choose Text only, Stopwatch (counts up), or Timer (counts down) with a length in minutes. Recent statuses are offered in the text box.

When you set a new status, the old line is struck through, slides under the new one and fades out. Several quick changes stack up to three older lines. The animation is timed in saved video frames, so it looks the same at every capture interval: at Capture every 5 seconds, a fade that lasts two seconds of video covers several real minutes. Stopwatches and timers count real time from when you set them, including while recording is paused or Windows sleeps. A finished timer shows how far over it is (for example +3:12) in amber. Turn on Repeat with breaks to alternate work blocks with breaks (for example 25 and 5 minutes) until you change the status. Timelapse notifies you when a timer ends and when each block changes. Clear status strikes the current line out without a new one.

A status can be set or changed at any time, including before and during a recording; it appears from the next saved frame and immediately in the preview. Corner, text size and style (Shadow only, or Edge fade, a dark strip that fades from the screen edge) are in the same window, with a zoomed preview of the chosen corner; they are remembered and fixed while recording. Every file of a recording shows the status. Each saved video, or each part of a split recording, also gets a `-status.txt` file listing when each status appeared in video time (for example `0:12 Shower (stopwatch)`). You can paste the list into a video description to add chapters. Very small video sizes cannot show the status; the window says so. The current status lasts until you change or clear it, or until Timelapse closes; it is not restored at the next start.

**Night camera:** For camera recordings, Advanced offers an optional Night camera (software blend) mode. It combines distinct camera frames over a period of time and adjusts brightness automatically. This can reduce random noise and blur movement; the camera's shutter settings remain unchanged. It uses an approximate linear-light blend of the camera's processed video, rather than raw sensor exposures, and cannot recover detail that the camera did not capture. Night mode is off by default.

Leave Blend duration on Auto, choose 1, 2, 5, 10, or 30 seconds, or use Custom for a duration from 1 to 30 seconds in exact milliseconds (for example, 1.5 or 15 seconds). These are requested software blending windows; camera frame timing and processing can vary. Auto starts with up to three seconds, then adjusts the blend duration using scene darkness and the observed camera-frame cadence, including when brightness correction reaches its limit. Every window stays within Capture every and the 30-second maximum. Night camera requires a capture interval of at least one second, including with Auto. A manual duration must not exceed Capture every; the app keeps an invalid selection visible and disables Record until corrected. Automatic brightness remains active with a manual duration.

Dark, Balanced, and Bright choose the desired brightness; Balanced is the default. Automatic brightness gives darker areas more influence than lamps or other bright patches, and lifts shadows while helping preserve highlight detail and color. Shadow gain is limited to 8× for one frame and can reach 64× with enough contributing frames and sufficiently stable signal. Temporal noise measurements limit amplification when successive frames remain noisy or correlated; the maximum is not applied to every scene. A small adaptive color-noise filter cleans up noisy blends while preserving luminance detail and protecting color edges. Clean blends skip this filter. These changes run inside the existing camera helper with no model, download, new thread, or additional camera captures.

More frames can reduce independent noise, but cannot remove every fixed-pattern artifact or recover shadows already clipped by the camera. Longer blends can reveal more dark detail at the cost of movement blur; for a stationary very dark scene, try Capture every 10 or 30 seconds with Auto blend duration. The last-blend “brightness target limited” note compares the whole-image average with the selected reference; it can also appear in mixed lighting even when darker areas reach their target. See [Night engineering and validation](https://github.com/Domincog/Timelapse/blob/v0.32.0/NIGHT_VALIDATION.md) for measured quality, processing cost, scientific limitations, and reproducible tests.

The initial full blend is preparation and does not consume a Stop after limit. Pause, Finish, or a time limit discards an unfinished blend; Resume starts a fresh one. Idle preview remains ordinary camera video. During recording, preview holds the most recently completed blend while the next one is prepared. Last-blend duration, camera-frame count, and shadow gain describe that completed result; the gain applies most strongly to the darkest tones. In collages, only the camera is blended; in two-file mode and with separate files, the desktop is captured near the end of the camera window. The files share playback timestamps, but their physical exposure periods differ. Slow cameras or processing can reduce the actual capture rate; full windows are never shortened to catch up. If no distinct camera frame arrives within a window, choose a longer blend and capture interval.

**Time compression:** Advanced offers an optional configuration for saving fewer frames during selected stretches. Off is the default. Scene is quiet uses image-change checks; Inside scheduled ranges follows your recording-time ranges regardless of movement; Quiet scene + schedule requires both a scheduled range and a quiet scene. All files from one recording always follow the same timing decision. Output plays at the selected playback FPS, and omitted source moments cannot be recovered from the resulting video.

Choose a maximum extra speed from 2x to 64x. At Capture every 5 seconds, 4x targets one saved frame every 20 seconds. Expand Fine tuning for transition duration and quiet-scene sensitivity; ordinary trigger, speed, waiting-time and schedule controls stay visible. Fine tuning starts collapsed and its caption identifies nondefault values.

Speed transitions use approximately 0.5, 1, or 2 seconds of **video time** (15, 30, or 60 saved frames at the default 30 fps). Frame counts adapt to the selected FPS, rounded to at least one frame. Their recording-time duration depends on capture interval and speed. Known manual ranges ramp up and reserve time to slow down before their end; short ranges may reach only a small part of the chosen maximum. Processing delays can interrupt the planned ramp. This changes capture timing; it does not create interpolated images or crossfades.

Add up to 16 ranges using offsets from recording-ready time, optionally repeating them at a chosen active duration. Overlapping or touching ranges merge. Initial preparation and pauses do not count, including for repeats: a repeating 24-hour schedule follows active recording time, not midnight or the wall clock. Stop after continues to count the full active recording time. Pause/resume and source/layout changes restart compression from the normal interval.

Quiet modes request small raw-image checks once per second, including while hidden, and wait for the chosen quiet duration (two minutes by default). Change in either selected source prevents quiet-mode compression. These checks measure image changes, not people or semantic importance. Very small, low-contrast, repetitive, or brief changes between checks may be missed. Desktop checks omit the cursor. Noise can conservatively prevent acceleration. An unavailable or stale check restores the normal capture interval and has its own status detail; a busy capture, encoder, or disk can delay checks.

Fine tuning offers Low, Standard and High change sensitivity for quiet modes. Standard keeps the existing behavior. Low tolerates more small image variation, which can also miss subtle activity; High responds to smaller changes and can stay at normal speed because of noise. All levels retain the same once-per-second checks and stale-result handling. This setting does not adjust the person detector.

Detected change returns directly to the normal interval rather than continuing a long slowdown. The next frame still respects Capture every, so return is not an immediate-event capture guarantee. Night camera checks use raw images, keep each blend bounded by the original Capture every, and never shorten an in-progress blend. Its response also includes the remaining or newly started full blend window. Current target cadence and actual last-check age are shown in Advanced. Off and manual-only modes do no image-change checking; automatic checking adds work, so fewer encoded frames does not by itself establish lower total CPU use or energy consumption.

**Optional person checks:** Time compression also offers No person detected (camera) and No person + schedule (camera). They check only the selected camera and can accelerate when other objects move. People or activity shown only on the desktop are not checked. All saved files still share the same cadence. Choose how long no person must be detected before acceleration; two minutes is the default. Distinct eligible camera checks must span that full active duration. A detected person, missing or failed check, or stale image clears that history and returns toward the next normal capture deadline. A full Night exposure still finishes before its frame can be saved.

Treat uncertain checks as no person is enabled by default in Time compression. Completed uncertain checks, including poor-detail images, count toward the selected no-person duration and can accelerate recording. The status says No person detected (uncertain) when that policy qualifies. Uncheck the option to keep normal cadence and clear absence history whenever a result is uncertain; those results then display Person check uncertain. The choice is remembered and fixed for a recording. Missing, stale or failed checks always display Checks unavailable and keep normal cadence.

Qualified no-person checks use the same 0.25 model-score threshold as person detection, with an image-detail check. Lower background scores qualify in an otherwise clear empty scene. These scores are model outputs, not calibrated probabilities. In either uncertainty mode, the selected no-person duration must pass before the capture interval gradually increases toward the selected multiplier.

Choose Manage detector, then Download / reinstall to obtain the optional local NanoDet-m model and CPU runtime (about 3.7 MiB installed). Opening the app, selecting a person mode, and pressing Record never download it. The default installer and portable ZIP do not contain this payload. It is stored in `LocalAppData\Timelapse\Person\NanoDet-r1` for both installed and portable users. Manage can remove it while recording is stopped. Uninstall preserves this optional per-user data with your preferences; remove it through Manage if wanted. Installation does not enable the feature.

Person checks run locally, at most once per second, after recording begins. Camera thumbnails are not sent over the network or saved separately. No detector is loaded for ordinary preview, Off, either Quiet mode, Manual schedule, or while paused. A separate owned process contains detector crashes and hangs; recording continues at its normal cadence if the model is missing, invalid or unavailable. Restart recording to retry a detector that failed. The app verifies the exact published worker size and SHA-256 before launch. Checks and their extra raw camera reads use CPU and memory while enabled; saving fewer frames does not guarantee reduced total power.

The detector can miss people, especially when small, obscured, unusual or poorly lit, and brief visits between checks can be skipped. With Treat uncertain checks as no person enabled, blank or low-detail images can accelerate recording after the waiting period. Uncheck it to suppress acceleration for those uncertain images; this is not a general blocked-lens detector. This feature is a recording convenience, not an occupancy or security guarantee. The normal capture interval still limits how promptly a newly detected person appears in the saved video.

**Record only while someone is there:** Time compression also offers No person detected: pause capture (camera), a stricter person mode. It saves frames at your normal Capture every interval while a person is detected in the camera, and saves nothing once nobody has been detected for the Keep recording after a person leaves time (set 1 second to stop almost at once). When someone appears again, the next frame is captured promptly instead of waiting out a slowed schedule. There is no speed ramp; the video cuts from the last frame before the break to the first frame after it. The first frame is saved when recording starts, before the first check completes. Missing, failed or stale checks keep recording normally, and Treat uncertain checks as no person decides whether uncertain checks can pause capture. Active time keeps counting while capture is paused, so Stop after and file splits follow the clock; a part with no frames creates no file. It uses the same optional detector, limits and privacy as the person modes above.

The installer keeps Timelapse in your user account's `LocalAppData\Programs\Timelapse` directory and adds Start menu and uninstall entries. Start with Windows is optional and starts the app in the tray without recording. Exit Timelapse from its tray menu before upgrading or uninstalling. Uninstalling preserves your recordings and settings.

Tab moves between controls. Alt + H changes the save folder while idle, and Alt + O opens it. Alt + C opens Time compression, including when Advanced is collapsed. In its schedule editor, Alt + R focuses the ranges list and Alt + M removes the selected range; changes take effect only after OK. During recording, compression settings are read-only and Enter from the ranges list closes the view.

In the collage preview, Space cycles between sources, arrow keys move the selected source, and Shift + arrow keys resize it. Using these keys while dragging ends the current drag and preserves the keyboard edit. Reset layout restores the preset.

On smaller work areas, the settings panel scrolls independently while the preview and recording controls stay visible. When the work area cannot fit the preview's minimum height, the whole window scrolls to keep every control reachable. Shift + wheel scrolls horizontally, and keyboard navigation brings focused controls into view.

Custom-value, Playback & shortcuts, Time compression, Watermark and Status Details dialogs also support these wheel gestures. Closed dropdowns retain their values while you scroll an overflowing dialog; open dropdowns, the scheduled-ranges list and the Details report keep their own scrolling. Wheel amount follows your Windows settings. In a constrained Details window, Tab brings the focused action into view.

The quality selector offers Smaller file, Balanced, and More detail. Balanced is the default. More detail prioritizes image detail; its file size depends on the scene and can grow substantially with motion, texture, or frequent cuts.

Expand Advanced to choose an encoding mode separately from resolution and quality:

| Encoding | Use |
| --- | --- |
| Compatible H.264 | Original software encoding settings and broad playback support. This remains the default. |
| Efficient H.264 | Tuned software encoding with a bitrate target and longer keyframe spacing. A useful starting point for long recordings. |
| Hardware H.264 | Uses an available hardware encoder to reduce CPU work, with broad H.264 playback support. |
| Hardware HEVC | Uses an available hardware HEVC encoder. Playback requires a compatible player or installed HEVC decoder. |
| Quality H.264 | Software encoding that prioritizes detail. Static screens can produce small files; frequent changes can produce much larger files. |
| AV1 | Built-in software AV1 encoding for the smallest files at the same or better measured detail. Uses noticeably more processor time; playback needs AV1 support. |

**AV1:** Timelapse includes the libaom AV1 encoder, so this mode works on any supported PC without a special GPU; it runs on the processor rather than offloading work to graphics hardware. Each quality level keeps a fixed quality target, and desktop text is detected and coded with AV1's screen-content tools. In the synthetic measurements below, Balanced AV1 files were 2–13 times smaller than Compatible H.264 Balanced files while measuring higher detail, with the largest savings on desktop and text content and the smallest on camera-like noise. A scene that changed completely at every capture produced a larger file than the bitrate-targeted H.264 modes, with much more detail, as fixed-quality modes do.

AV1 encodes each frame as it is captured, using up to four processor threads. In testing on an 8-core laptop processor, a 1080p frame took about 40–130 ms on desktop and camera-like scenes and about 450 ms when the whole picture changed, using roughly 4–15 times the processor time of Efficient H.264. Keyframes, every ten seconds of playback, take longer (over a second at 1080p), and completely changing 4K frames took about two seconds each. When encoding cannot keep up with a short capture interval, capture slots are skipped as usual, and Pause or Finish can wait for a frame already being encoded. Each AV1 output used about 135 MiB of additional memory at 1080p (about 460 MiB at 4K). Desktop + camera (2 files) runs two encoders, and a collage with both separate files runs three.

AV1 playback requires an AV1-capable player or decoder. Windows playback may need the AV1 Video Extension. Some older players, editors and TVs cannot open these files. MP4 recovery mode requires H.264. Writing AV1 MP4 uses Windows Media Foundation and has been verified on Windows 11; Windows 10 AV1 MP4 writing has not been verified. If Windows cannot write AV1 MP4, recording reports an error when it starts and no file remains.

Hardware support depends on the computer and driver. If a hardware mode is unavailable, choose Compatible H.264 or Efficient H.264. The app verifies that hardware modes actually use a hardware encoder. Encoding and quality choices are saved and remain locked during a recording.

Efficient uses variable bitrate at all three quality levels. Its bitrate is a target, not a strict file-size cap. Quality H.264, AV1 and the hardware modes use fixed quantization settings: detailed scenes, noise and frequent changes can need substantially more data. A long capture interval reduces the number of frames but can also make consecutive frames less alike. The quality labels describe a tradeoff within each mode and do not promise identical image quality or file size across different codecs. Hardware encoding can reduce CPU use while keeping a dedicated GPU awake, so lower CPU use does not establish better battery life.

Desktop capture excludes this app's window on supported Windows versions. Minimize or close the window to stop preview updates while recording continues. Camera modes activate the camera for preview and recording. The app starts in Desktop mode so opening it does not silently activate a camera.

Switching sources or devices clears the previous preview until the new selection produces a frame.

Refresh keeps the selected camera and display when the device list changes. If a selected source is unavailable, choose a replacement or refresh after it returns. Record stays disabled while a source needed by the current layout is unavailable.

Recording follows the selected display's identity and current bounds. If the display is missing or changes while a recording frame is captured, that frame is rejected and recording stops, attempting to save earlier frames.

Saving keeps ownership of the original recording through finalization and the final filename change. An existing destination is never overwritten. If the filename change fails, the status message identifies the finished video retained at its `.recording.mp4` path. If finalization itself fails, any retained partial file may be incomplete. Save folders and preference files support long local Windows paths and Unicode names.

Frame and video-time statistics describe samples accepted by the encoder, including a sample accepted just before an MP4 recovery-section error. They remain session totals across file splits; multi-file recordings use the common count, with any difference described in the final report. These statistics do not guarantee that every accepted sample remains playable after a finalization failure or interruption.

## Build and verify

Install CMake 3.20 or later (3.21 or later for Visual Studio 2022) and Visual Studio 2019 or 2022 Build Tools with the Desktop development with C++ workload and Windows 10 SDK. The built-in AV1 encoder also needs Perl, which Git for Windows includes; the build finds it next to `git`, or set `-DTIMELAPSE_PERL_EXECUTABLE=<path>`.

The first build downloads the pinned [libaom](https://aomedia.googlesource.com/aom/) 3.15.1 release and the NASM 3.02 assembler used to build it, and verifies each against a fixed SHA-256 digest (see `third-party/libaom.cmake`). This adds a few minutes to the first build only. For an offline build, download the same files and pass `-DTIMELAPSE_LIBAOM_ARCHIVE=<path>\libaom-3.15.1.tar.gz -DTIMELAPSE_NASM_ARCHIVE=<path>\nasm-3.02-win64.zip` when configuring; they are verified the same way. libaom is configured in its own isolated build tree (`build/aom`) and compiled into the app; nothing is installed system-wide. Keep the build path reasonably short, because libaom's intermediate file paths are long.

Run from PowerShell:

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

- One display and one camera at a time, saved as up to three files that share one capture schedule; 720p, 1080p, or custom output within the size limits above, preserving source aspect ratios. Camera input is bounded at 720p for smaller outputs and up to 1080p for larger outputs, subject to camera support. Desktop output can retain more detail when the selected display provides it.
- The desktop must remain unlocked and awake. Protected content may appear black. A removed display or unavailable camera stops recording and attempts to save captured frames.
- MP4 is finalized by Finish, Exit, an automatic time limit, or a configured file-split boundary. Closing the window keeps recording. An unfinished ordinary MP4 can be unplayable after forced termination or power loss. Optional H.264 recovery mode improves the chance of playing completed sections after interruption, with the limits described above.
- Windows N requires the Media Feature Pack. The app and installer are currently unsigned.
- Camera compatibility and performance vary by device; automated media tests use generated frames and do not establish physical-camera compatibility.
- Display identity uses a Windows monitor interface when available, with a GDI display-name fallback. The fallback cannot distinguish a replacement using the same name; changes that disappear and return entirely within one capture can escape detection.

The app retains the latest preview and camera frame, and writes samples incrementally. Preview runs at two frames per second when idle and one while recording; minimized windows do not generate preview frames. There is no growing in-memory recording buffer. The app requests that Windows stay awake while recording or saving. It releases the request while paused and after saving finishes. Manual locking is still respected.

Desktop capture reuses its recording surface alongside a bounded preview surface, avoiding repeated native allocations as capture and preview sizes alternate. The extra preview surface uses at most 900 KiB. This keeps the recording-sized surface resident between saved frames while recording is visible, trading retained memory for reuse. Pause and Finish release the native surfaces even when the window stays visible; continued preview recreates only what it needs. Hidden idle or paused sessions and switching away from desktop capture release them too.

A single full-frame source already matching the output size avoids unnecessary composition scratch allocation and background clearing. Sources that need no scaling copy four pixels at a time using the SSE2 instructions available on supported x64 processors, including matching-size layers within a collage. Pixel values, alpha handling, and scaling/letterboxing are unchanged.

While a night window is active, its camera helper blends up to five distinct contributions per second, at the selected input limit of up to 720p or 1080p. Accumulation buffers are bounded and sized to the delivered input. Intermediate blend data stays in the helper; only the completed blend crosses to the recorder. During initial Night preparation, raw preview can reuse a fresh contribution from the same helper iteration instead of converting it again. After the first completed blend, preview holds the processed image. These optional buffers are released when night recording is cancelled or finished. Hardware camera capture/conversion can still consume resources independently of the blend processing.

Camera access runs in a private helper process launched from the same executable. A stuck camera driver can be stopped without trapping the app in shutdown. The helper exits with its parent and uses only local shared memory; there are no network services.

The recording worker sleeps until capture, preview, an enabled activity check, a schedule boundary, a clock update or a command is due. Hidden idle and paused sessions have no periodic worker tick. Outside an active Night window, the camera helper converts and copies pixels only when requested, including the optional activity checks; its camera reader continues receiving current samples. During a Night window, the helper processes distinct contributions locally at up to five per second and shares those raw samples with activity checks. Completed Night pixels take priority over optional observation work. This reduces unnecessary work between captures without making an old frame appear fresh.

The interface applies control, text, tray, and paint updates only when their inputs change. Hidden or minimized windows defer visual updates until restored, while recording failures and Exit still receive regular status checks. The GUI's 200 ms status timer remains active; these changes reduce repeated work, not timer wakeups.

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

The AV1 mode was measured with the same four clips, machine and method (Balanced quality; CPU is the mean of two alternating runs, which produced identical files). Pooled luma PSNR is from independent decoding by both Windows and FFmpeg/dav1d:

| Scene | File size (MB): Compatible / Efficient / AV1 | Luma PSNR (dB): Compatible / Efficient / AV1 | Encoding CPU (s): Compatible / Efficient / AV1 |
| --- | --- | --- | --- |
| Mostly static desktop text | 0.878 / 0.349 / 0.066 | 54.54 / 53.69 / 70.00 | 4.04 / 3.52 / 15.78 |
| Scrolling and cuts | 3.105 / 2.875 / 0.317 | 52.44 / 53.52 / 56.44 | 5.37 / 4.12 / 29.53 |
| Textured motion and noise | 6.385 / 5.110 / 2.911 | 37.96 / 38.50 / 38.85 | 23.41 / 9.38 / 64.12 |
| Large changes every frame | 7.042 / 5.213 / 27.701 | 32.20 / 31.90 / 38.53 | 31.95 / 16.38 / 239.73 |

AV1 Balanced also had the lowest small-text edge error (0.27 and 1.55 versus 1.66 and 2.22 for Compatible on the two screen scenes). On the last scene its fixed quality target kept far more detail (block SSIM 0.945 versus 0.757) at a much larger size than the bitrate-targeted modes; Quality H.264 needed 53.5 MB for 39.25 dB there. AV1's three quality levels measured:

| Scene | Smaller file | Balanced | More detail |
| --- | --- | --- | --- |
| Mostly static desktop text | 0.058 MB, 66.88 dB | 0.066 MB, 70.00 dB | 0.072 MB, 72.29 dB |
| Scrolling and cuts | 0.193 MB, 53.41 dB | 0.317 MB, 56.44 dB | 0.519 MB, 59.41 dB |
| Textured motion and noise | 1.632 MB, 36.15 dB | 2.911 MB, 38.85 dB | 6.337 MB, 40.83 dB |
| Large changes every frame | 11.603 MB, 34.79 dB | 27.701 MB, 38.53 dB | 39.376 MB, 40.98 dB |

Optional tools generate synthetic desktop text, scrolling/cuts, camera-like motion/noise, and large changes between captures. They do not capture a display or camera. Build them separately:

```powershell
cmake -S . -B build-benchmark -G "Visual Studio 16 2019" -A x64 -DTIMELAPSE_BUILD_BENCHMARKS=ON
cmake --build build-benchmark --config Release --target encoding_benchmark encoding_quality_verifier
.\build-benchmark\Release\encoding_quality_verifier.exe --self-test
.\build-benchmark\Release\encoding_benchmark.exe efficient balanced 0 1920 1080 180 screen.mp4
.\build-benchmark\Release\encoding_quality_verifier.exe screen.mp4 0 1920 1080 180
```

Use a new output filename for every run. Modes are `compatible`, `efficient`, `hardware-h264`, `hardware-hevc`, `quality-h264`, and `av1`; qualities are `compact`, `balanced`, and `detail`. Scenes `0`, `1`, and `2` exercise a mostly static screen, scrolling/cuts, and textured motion/noise. Scene `3` stresses large changes and frequent cuts between captures. A 180-frame clip crosses the five-second keyframe boundary of the H.264 modes other than Compatible; AV1 places keyframes ten seconds apart.

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
- `third-party/`: the pinned libaom build recipe (`libaom.cmake`) and its license and patent texts.
- `person-pack/`: separately built optional detector, pinned dependency instructions, licenses and model/protocol checks. A normal app build needs no model download.

After building and testing, run `./package.ps1` to create both archives in `packages/`. The release ZIP is intended for GitHub Releases. Extract the source ZIP into an empty folder, then run the build commands from that folder; build outputs, test recordings, and local settings are excluded.

To also create the Windows setup EXE for the current version, supply an installed or portable Inno Setup 7 compiler:

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

## Thumbnail resize attribution

The optional camera-thumbnail preprocessing adapts Tencent/ncnn's scalar resize arithmetic (Copyright 2018 Tencent). Changes add bounded BGRA-to-BGR input, fixed coefficient storage and allocation-free aspect resizing. The main recorder contains this small adaptation; the full inference runtime is only in the optional worker. The upstream license and notices follow.

Tencent is pleased to support the open source community by making ncnn available.
Copyright (C) 2017 Tencent.  All rights reserved.
If you have downloaded a copy of the ncnn binary from Tencent, please note that the ncnn binary is licensed under the BSD 3-Clause License.
If you have downloaded a copy of the ncnn source code from Tencent, please note that ncnn source code is licensed under the BSD 3-Clause License, except for the third-party components listed below which are subject to different license terms.  Your integration of ncnn into your own projects may require compliance with the BSD 3-Clause License, as well as the other licenses applicable to the third-party components included within ncnn.
A copy of the BSD 3-Clause License is included in this file.

Other dependencies and licenses:

Open Source Software Licensed Under the zlib License:
The below software in this distribution may have been modified by Tencent (“Tencent Modifications”). All Tencent Modifications are Copyright (C) 2017 Tencent.
----------------------------------------------------------------------------------------
1. neon_mathfun.h
Copyright (C) 2011 Julien Pommier

2. sse_mathfun.h
Copyright (C) 2007 Julien Pommier

3. avx_mathfun.h
Copyright (C) 2012 Giovanni Garberoglio
Interdisciplinary Laboratory for Computational Science (LISC)
Fondazione Bruno Kessler and University of Trento
via Sommarive, 18
I-38123 Trento (Italy)


Terms of the zlib License:
---------------------------------------------------
Copyright (c) <year> <copyright holders>

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.



Open Source Software Licensed Under the BSD 2-Clause License:
The below software in this distribution may have been modified by Tencent (“Tencent Modifications”). All Tencent Modifications are Copyright (C) 2017 Tencent.
----------------------------------------------------------------------------------------
1. squeezenet  1.1
Copyright (c) 2016 Forrest N. Iandola and Matthew W. Moskewicz and Khalid Ashraf and Song Han and William J. Dally and Kurt Keutzer
All rights reserved.

2. caffe.proto  master
All contributions by the University of California:
Copyright (c) 2014-2017 The Regents of the University of California (Regents)
All rights reserved.

All other contributions:
Copyright (c) 2014-2017, the respective contributors
All rights reserved.


Terms of the BSD 2-Clause License:
--------------------------------------------------------------------
Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.



Open Source Software Licensed Under the BSD 3-Clause License:
The below software in this distribution may have been modified by Tencent (“Tencent Modifications”). All Tencent Modifications are Copyright (C) 2017 Tencent.
----------------------------------------------------------------------------------------
1. android.toolchain.cmake  master
Copyright (c) 2010-2011, Ethan Rublee
Copyright (c) 2011-2014, Andrey Kamaev
All rights reserved.


Terms of the BSD 3-Clause License:
--------------------------------------------------------------------

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
Neither the name of [copyright holder] nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## AV1 encoder notices

The AV1 encoding mode compiles in libaom 3.15.1 from the Alliance for Open Media, unmodified. Its license and the AOMedia patent license follow; the same texts are in `third-party/`.

```text
Copyright (c) 2016, Alliance for Open Media. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

```text
Alliance for Open Media Patent License 1.0

1. License Terms.

1.1. Patent License. Subject to the terms and conditions of this License, each
     Licensor, on behalf of itself and successors in interest and assigns,
     grants Licensee a non-sublicensable, perpetual, worldwide, non-exclusive,
     no-charge, royalty-free, irrevocable (except as expressly stated in this
     License) patent license to its Necessary Claims to make, use, sell, offer
     for sale, import or distribute any Implementation.

1.2. Conditions.

1.2.1. Availability. As a condition to the grant of rights to Licensee to make,
       sell, offer for sale, import or distribute an Implementation under
       Section 1.1, Licensee must make its Necessary Claims available under
       this License, and must reproduce this License with any Implementation
       as follows:

       a. For distribution in source code, by including this License in the
          root directory of the source code with its Implementation.

       b. For distribution in any other form (including binary, object form,
          and/or hardware description code (e.g., HDL, RTL, Gate Level Netlist,
          GDSII, etc.)), by including this License in the documentation, legal
          notices, and/or other written materials provided with the
          Implementation.

1.2.2. Additional Conditions. This license is directly from Licensor to
       Licensee.  Licensee acknowledges as a condition of benefiting from it
       that no rights from Licensor are received from suppliers, distributors,
       or otherwise in connection with this License.

1.3. Defensive Termination. If any Licensee, its Affiliates, or its agents
     initiates patent litigation or files, maintains, or voluntarily
     participates in a lawsuit against another entity or any person asserting
     that any Implementation infringes Necessary Claims, any patent licenses
     granted under this License directly to the Licensee are immediately
     terminated as of the date of the initiation of action unless 1) that suit
     was in response to a corresponding suit regarding an Implementation first
     brought against an initiating entity, or 2) that suit was brought to
     enforce the terms of this License (including intervention in a third-party
     action by a Licensee).

1.4. Disclaimers. The Reference Implementation and Specification are provided
     "AS IS" and without warranty. The entire risk as to implementing or
     otherwise using the Reference Implementation or Specification is assumed
     by the implementer and user. Licensor expressly disclaims any warranties
     (express, implied, or otherwise), including implied warranties of
     merchantability, non-infringement, fitness for a particular purpose, or
     title, related to the material. IN NO EVENT WILL LICENSOR BE LIABLE TO
     ANY OTHER PARTY FOR LOST PROFITS OR ANY FORM OF INDIRECT, SPECIAL,
     INCIDENTAL, OR CONSEQUENTIAL DAMAGES OF ANY CHARACTER FROM ANY CAUSES OF
     ACTION OF ANY KIND WITH RESPECT TO THIS LICENSE, WHETHER BASED ON BREACH
     OF CONTRACT, TORT (INCLUDING NEGLIGENCE), OR OTHERWISE, AND WHETHER OR
     NOT THE OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

2. Definitions.

2.1. Affiliate.  "Affiliate" means an entity that directly or indirectly
     Controls, is Controlled by, or is under common Control of that party.

2.2. Control. "Control" means direct or indirect control of more than 50% of
     the voting power to elect directors of that corporation, or for any other
     entity, the power to direct management of such entity.

2.3. Decoder.  "Decoder" means any decoder that conforms fully with all
     non-optional portions of the Specification.

2.4. Encoder.  "Encoder" means any encoder that produces a bitstream that can
     be decoded by a Decoder only to the extent it produces such a bitstream.

2.5. Final Deliverable.  "Final Deliverable" means the final version of a
     deliverable approved by the Alliance for Open Media as a Final
     Deliverable.

2.6. Implementation.  "Implementation" means any implementation, including the
     Reference Implementation, that is an Encoder and/or a Decoder. An
     Implementation also includes components of an Implementation only to the
     extent they are used as part of an Implementation.

2.7. License. "License" means this license.

2.8. Licensee. "Licensee" means any person or entity who exercises patent
     rights granted under this License.

2.9. Licensor.  "Licensor" means (i) any Licensee that makes, sells, offers
     for sale, imports or distributes any Implementation, or (ii) a person
     or entity that has a licensing obligation to the Implementation as a
     result of its membership and/or participation in the Alliance for Open
     Media working group that developed the Specification.

2.10. Necessary Claims.  "Necessary Claims" means all claims of patents or
      patent applications, (a) that currently or at any time in the future,
      are owned or controlled by the Licensor, and (b) (i) would be an
      Essential Claim as defined by the W3C Policy as of February 5, 2004
      (https://www.w3.org/Consortium/Patent-Policy-20040205/#def-essential)
      as if the Specification was a W3C Recommendation; or (ii) are infringed
      by the Reference Implementation.

2.11. Reference Implementation. "Reference Implementation" means an Encoder
      and/or Decoder released by the Alliance for Open Media as a Final
      Deliverable.

2.12. Specification. "Specification" means the specification designated by
      the Alliance for Open Media as a Final Deliverable for which this
      License was issued.
```
