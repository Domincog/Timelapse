# Timelapse

Timelapse is a free Windows app that turns hours of screen or webcam activity into a short video. It takes a picture every few seconds and saves the pictures as an MP4, so a whole afternoon of work, a painting session or a sunset plays back in a minute or two.

- Record your **screen**, your **webcam**, or **both at once** (picture-in-picture, side by side, or your own layout).
- Everything stays on your PC. No account, no upload, no ads.
- Small and self-contained: no browser engine, .NET or separate video encoder to install.

**[Download the latest version](https://github.com/Domincog/Timelapse/releases/latest)**: pick the `setup.exe` installer, or the portable `.zip` if you'd rather not install anything.

Requires Windows 10 (version 2004 or later) or Windows 11, 64-bit. The app isn't code-signed yet, so Windows SmartScreen may warn you the first time; choose **More info → Run anyway**. On Windows "N" editions, install the Media Feature Pack first.

## Quick start

1. Open Timelapse. It starts in **Desktop** mode and shows a live preview.
2. Under **Source**, choose what to record: Desktop, Camera, Desktop + camera, Side by side, Custom collage, or Desktop + camera (2 files).
3. Set **Capture every** to how often a picture is taken. The default, 5 seconds, turns one hour into about 24 seconds of video.
4. Press **Record**. Use **Pause** whenever you step away, and **Finish** when you're done.
5. Click **Open folder** to see your video. By default it's saved in `Videos\Timelapse`.

You can close the window while recording: Timelapse keeps going in the system tray (near the clock). Click the tray icon to bring it back, or right-click it for Pause, Finish and more.

**How long will my video be?** Divide the recording time by the capture interval to get the number of frames, then divide by 30 (frames per second). For example, 2 hours at one picture every 10 seconds is 720 frames, which plays for 24 seconds.

## Settings

The settings panel on the right has three tabs. A dot on a tab means something there differs from the default; a red dot means something needs fixing before you can record. Hover over most settings for a short explanation.

### Capture tab

| Setting | What it does |
| --- | --- |
| **Source** | What goes in the video. Desktop + camera puts the webcam in a corner of the screen. In a collage you can drag a source to move it, drag its corner to resize it, and use **Bring forward** to change which one is on top. **Desktop + camera (2 files)** saves the screen and the webcam as two separate videos instead. |
| **Display / Camera** | Which screen and which webcam to use. Press **Refresh** if you plug one in. |
| **Also save files** | With a combined layout, also save the full screen and/or the full webcam as their own videos at the same time. |
| **Capture every** | Time between pictures: 1 to 60 seconds, or **Custom** (from 0.1 seconds up to 24 hours). Longer gaps make shorter, faster videos. |
| **Video size** | 720p (default), 1080p, your screen's or camera's own size, or a custom size, including portrait and square. |
| **Video quality** | Extra small file, Smaller file, Balanced (default) or More detail. Busy, fast-changing scenes make bigger files. |
| **Save to** | Where videos go. **Change...** picks a folder; **Open folder** opens it. |
| **Status** | Show what you're doing in a corner of the video. See [Status in the video](#status-in-the-video). |

### Recording tab

| Setting | What it does |
| --- | --- |
| **Stop after** | Finish and save automatically after this much recording time (pauses don't count). Default: Never. |
| **Split files every** | Save the recording in parts, for example one file per hour, so a crash or power cut can only affect the part in progress. Parts are named `...-part-000001.mp4`, `...-part-000002.mp4` and so on. Default: Never. |
| **Start delay** | A countdown (up to 5 minutes) after pressing Record, so you can get ready or hide the window first. |
| **Stop on low disk space** | Stops and saves before the drive fills up. On by default; turn it off only for folders that can't report free space, such as some network drives. |
| **Show desktop cursor** | Include the mouse pointer in screen recordings. On by default. |
| **Time compression...** | Save fewer pictures during boring stretches so they fly by. See [Time compression](#time-compression). |
| **Night camera** | Brighten dark webcam footage by blending several camera frames into each picture. See [Night camera](#night-camera). |

### Output tab

| Setting | What it does |
| --- | --- |
| **Encoding** | The video format. **Compatible H.264** (default) plays almost everywhere. **Efficient H.264** usually makes smaller files. **Hardware H.264 / HEVC** use your graphics chip to save CPU. **Quality H.264** keeps more detail. **SVT-AV1** makes the smallest files but uses more CPU, and needs an AV1-capable player (Windows may need the free *AV1 Video Extension*). HEVC needs an HEVC-capable player. |
| **Encoder settings...** | For experts: AV1 speed preset (0–11, default 6), a fixed AV1 quality level (CRF), or a target bitrate. |
| **MP4 recovery mode** | H.264 only. Writes the video in small sections so that if the app is forced to close, what was recorded so far is more likely to play. Off by default because some editors don't support this kind of MP4. |
| **Watermark...** | Stamp the video with the elapsed time, the date and time, and/or the playback speed, in any corner or position. |
| **Playback & shortcuts...** | Playback frame rate (1–120 fps, default 30; higher means a shorter, faster video), plus optional global keyboard shortcuts for Pause/Resume, Stop and save, and Set status that work even when Timelapse is hidden. |
| **Reset all settings...** | Puts every setting back to its default (your chosen display and camera stay selected). |

Settings are remembered between sessions.

## Changing settings while recording

Most settings can be changed while you record or while paused, and apply to the video from the next picture: Source and the collage layout, Display, Capture every, Show desktop cursor, Stop after, the length of split parts, Stop on low disk space, Night camera, Time compression, Watermark, status appearance and the keyboard shortcuts. If a change can't work with the current recording (for example, a Stop after time that has already passed), Timelapse tells you and keeps recording as before.

A few things are fixed once recording starts, because changing them would need a new file: video size and quality, encoding and encoder settings, MP4 recovery mode, playback frame rate, the camera, the save folder, which files are saved, and turning file splitting on or off. Change those before you press Record.

## Status in the video

Click **Set status...** (or use the tray menu or a shortcut) and type a short note such as "Lunch" or "Working on chapter 3". It appears in a corner of the video, like a game's event feed: when you set a new one, the old one is crossed out and fades away. A status can also be a **Stopwatch** that counts up or a **Timer** that counts down, optionally repeating with breaks (for example 25 minutes of work, 5 minutes of break). Timelapse notifies you when a timer ends.

In the same window you can choose the corner, size and style, and turn on **Also save a status list (.txt)**. That writes a text file next to each video listing when each status appeared, for example `0:12 Lunch`, ready to paste into a YouTube description as chapters.

## Time compression

Time compression speeds up the dull parts by taking pictures less often while nothing is happening, then returning to normal as soon as something changes. Choose when to speed up:

- **Scene is quiet**: when the picture hasn't changed for a while (two minutes by default).
- **Inside scheduled ranges**: during set periods of the recording, such as minutes 30–90, optionally repeating.
- **Quiet scene + schedule**: both at once.
- **No person detected (camera)** and **No person + schedule (camera)**: when nobody is visible on the webcam.
- **No person detected: pause capture (camera)**: record only while someone is on camera.

Choose how much faster it can go (2× to 64×). The speed-up ramps in and out smoothly. The quiet check looks for changes in the picture, not their importance, so very small or brief changes can be missed.

**Person detection** needs a small optional download (about 3.7 MB). In Time compression, choose **Manage detector → Download**. It runs entirely on your PC; camera images are never uploaded or saved separately. It can miss people who are small, partly hidden or poorly lit, so don't rely on it for security.

## Night camera

For dark rooms or night scenes, Night camera combines many webcam frames into each picture and brightens it automatically, which cuts down grainy noise. Moving things will look blurred. Leave **Blend duration** on Auto, or pick 1–30 seconds; it can't be longer than Capture every. **Auto brightness** offers Dark, Balanced (default) and Bright. For a very dark, still scene, try capturing every 10 or 30 seconds. It brightens what the camera captures; it can't recover detail the camera never saw.

## Tips

- **Desktop capture hides the Timelapse window itself**, so it won't appear in your recording. Minimize it to save a little CPU; recording continues.
- **The computer must stay awake and unlocked** for screen recording. Timelapse asks Windows to stay awake while recording; locking the screen or protected video (some streaming apps) can show as black.
- **If something goes wrong**, Timelapse stops and saves what it already recorded, and the status line explains why. Click **Details** to read or copy the full message, and **Show files** to find the saved videos.
- **Stopping safely:** always use **Finish**, Exit from the tray menu, or Stop after. A normal MP4 that's cut off by a crash or power loss may not play; turn on **Split files every** or **MP4 recovery mode** for long sessions.
- **Several files at once** (two-file mode or Also save files) each need their own encoding work, so they use more CPU and disk space.
- **Very short capture intervals** with large videos may be more than your PC can keep up with. Timelapse then simply skips pictures rather than falling behind.

## Keyboard shortcuts

| Keys | Action |
| --- | --- |
| Ctrl + Tab, Ctrl + Shift + Tab | Switch settings tabs |
| Alt + underlined letter | Jump to that setting (for example Alt + T for Stop after) |
| Alt + C | Open Time compression |
| Alt + I | Status details |
| Space / arrow keys / Shift + arrows | In a collage preview: select, move, and resize a source |
| Your own global shortcuts | Pause/Resume, Stop and save, Set status (set them in Playback & shortcuts) |

## Install, update and uninstall

The installer puts Timelapse in your user account (`%LocalAppData%\Programs\Timelapse`) and adds it to the Start menu. It can optionally start with Windows (in the tray, without recording). To update, exit Timelapse from its tray menu and run the new installer. Uninstalling keeps your recordings and settings. The portable ZIP needs no installation: extract it and run `Timelapse.exe`.

## Known limitations

- One display and one webcam per recording. No audio.
- Removing the display or camera being recorded stops the recording (what was recorded is saved).
- Camera compatibility varies by device.
- Writing AV1 files has been verified on Windows 11 but not on Windows 10.

## For developers

### Build

Requirements: Visual Studio 2019 or 2022 (or Build Tools) with the **Desktop development with C++** workload and a Windows 10 SDK, and CMake 3.20 or later (3.21 or later for Visual Studio 2022). From this folder in PowerShell:

```powershell
.\build.ps1 -Test
```

This builds, runs the tests and writes `dist\Timelapse.exe`, `dist\Timelapse-portable.zip` and `dist\SHA256SUMS.txt`. The first build downloads the pinned [SVT-AV1 v4.2.0](https://gitlab.com/AOMediaCodec/SVT-AV1/-/tree/v4.2.0) source and the NASM 3.02 assembler and checks them against fixed SHA-256 digests (see `third-party/svt-av1.cmake`). For an offline build, pass `-SvtAv1Archive <path>\SVT-AV1-v4.2.0.tar.gz -NasmArchive <path>\nasm-3.02-win64.zip`. Keep the build path short, because dependency paths can get long.

Without PowerShell scripts (use `Visual Studio 17 2022` for VS 2022; add `-DTIMELAPSE_SVT_AV1_ARCHIVE=...` and `-DTIMELAPSE_NASM_ARCHIVE=...` for offline builds):

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The app is then `build\release\Timelapse.exe`. The tests use generated images and real MP4 encoding and decoding. `engine_tests` also records the real desktop and needs an unlocked interactive session; add `-E "^engine_tests$"` to skip it. Launching with `--inspect-ui` allows screenshots of the app window for visual checks.

### Packaging

After a Release build, `.\package.ps1` writes the portable ZIP, a source ZIP and checksums to `packages\`. Add `-InstallerCompiler '<path>\ISCC.exe'` (Inno Setup 7) to also build the installer. Packaging regression checks: `tests\build_tests.ps1`, `tests\package_tests.ps1` and, with an Inno Setup compiler, `tests\installer_tests.ps1` (run each with `powershell -NoProfile -ExecutionPolicy Bypass -File`).

### Encoding benchmarks

Optional tools measure file size, CPU time and quality on generated test scenes (they never capture your screen or camera). Configure with `-DTIMELAPSE_BUILD_BENCHMARKS=ON` and build the `encoding_benchmark` and `encoding_quality_verifier` targets; `tools\verify-encoding-quality.ps1` adds an independent FFmpeg check. See [AV1 validation](AV1_EFFICIENCY.md) and [Night validation](NIGHT_VALIDATION.md) for measured results and methods.

### Repository layout

- `src/`: the app (capture, layout, encoding, user interface).
- `tests/`: automated tests.
- `tools/`: optional verification scripts.
- `installer/`: Inno Setup script for the per-user installer.
- `third-party/`: the pinned SVT-AV1 build recipe, licenses and notices.
- `person-pack/`: the separately built optional person detector.

## Third-party notices

Timelapse includes code from the projects below. Their licenses require these notices.

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

The AV1 encoding mode compiles in the pinned SVT-AV1 v4.2.0 release, unmodified. Its Clear BSD and BSD-2-Clause license texts, the AOMedia patent license, and the fastfeat and safestringlib dependency notices follow. The same texts are in the source archive's third-party directory. This README is included in the portable ZIP and installed app.

```text
BSD 3-Clause Clear License
The Clear BSD License

Copyright (c) 2021, Alliance for Open Media

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted (subject to the limitations in the disclaimer below)
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the distribution.

3. Neither the name of the Alliance for Open Media nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

```text
Copyright (c) 2019, Alliance for Open Media. All rights reserved.

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
**Alliance for Open Media Patent License 1.0**

 1. **License Terms.**

    **Patent License.** Subject to the terms and conditions of this License, each
     Licensor, on behalf of itself and successors in interest and assigns,
     grants Licensee a non-sublicensable, perpetual, worldwide, non-exclusive,
     no-charge, royalty-free, irrevocable (except as expressly stated in this
     License) patent license to its Necessary Claims to make, use, sell, offer
     for sale, import or distribute any Implementation.

     **Conditions.**

    *Availability.* As a condition to the grant of rights to Licensee to make,
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

    *Additional Conditions.* This license is directly from Licensor to
       Licensee.  Licensee acknowledges as a condition of benefiting from it
       that no rights from Licensor are received from suppliers, distributors,
       or otherwise in connection with this License.

    **Defensive Termination**. If any Licensee, its Affiliates, or its agents
     initiates patent litigation or files, maintains, or voluntarily
     participates in a lawsuit against another entity or any person asserting
     that any Implementation infringes Necessary Claims, any patent licenses
     granted under this License directly to the Licensee are immediately
     terminated as of the date of the initiation of action unless 1) that suit
     was in response to a corresponding suit regarding an Implementation first
     brought against an initiating entity, or 2) that suit was brought to
     enforce the terms of this License (including intervention in a third-party
     action by a Licensee).

    **Disclaimers.** The Reference Implementation and Specification are provided
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
     NOT THE OTHER PARTRY HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

2. **Definitions.**

     **Affiliate.**  "Affiliate" means an entity that directly or indirectly
     Controls, is Controlled by, or is under common Control of that party.

    **Control.** "Control" means direct or indirect control of more than 50% of
     the voting power to elect directors of that corporation, or for any other
     entity, the power to direct management of such entity.

    **Decoder.**  "Decoder" means any decoder that conforms fully with all
     non-optional portions of the Specification.

    **Encoder.**  "Encoder" means any encoder that produces a bitstream that can
     be decoded by a Decoder only to the extent it produces such a bitstream.

    **Final Deliverable.**  "Final Deliverable" means the final version of a
     deliverable approved by the Alliance for Open Media as a Final
     Deliverable.

    **Implementation.**  "Implementation" means any implementation, including the
     Reference Implementation, that is an Encoder and/or a Decoder. An
     Implementation also includes components of an Implementation only to the
     extent they are used as part of an Implementation.

    **License.** "License" means this license.

    **Licensee.** "Licensee" means any person or entity who exercises patent
     rights granted under this License.

    **Licensor.**  "Licensor" means (i) any Licensee that makes, sells, offers
     for sale, imports or distributes any Implementation, or (ii) a person
     or entity that has a licensing obligation to the Implementation as a
     result of its membership and/or participation in the Alliance for Open
     Media working group that developed the Specification.

    **Necessary Claims.**  "Necessary Claims" means all claims of patents or
      patent applications, (a) that currently or at any time in the future,
      are owned or controlled by the Licensor, and (b) (i) would be an
      Essential Claim as defined by the W3C Policy as of February 5, 2004
      (https://www.w3.org/Consortium/Patent-Policy-20040205/#def-essential)
      as if the Specification was a W3C Recommendation; or (ii) are infringed
      by the Reference Implementation.

     **Reference Implementation.** "Reference Implementation" means an Encoder
      and/or Decoder released by the Alliance for Open Media as a Final
      Deliverable.

     **Specification.** "Specification" means the specification designated by
      the Alliance for Open Media as a Final Deliverable for which this
      License was issued.
```

```text
Copyright (c) 2006, 2008 Edward Rosten
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:


	*Redistributions of source code must retain the above copyright
	 notice, this list of conditions and the following disclaimer.

	*Redistributions in binary form must reproduce the above copyright
	 notice, this list of conditions and the following disclaimer in the
	 documentation and/or other materials provided with the distribution.

	*Neither the name of the University of Cambridge nor the names of
	 its contributors may be used to endorse or promote products derived
	 from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

```text
MIT License

Copyright (c) 2014-2018 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

================================================================================

Copyright (C) 2012, 2013 Cisco Systems
All rights reserved.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```
