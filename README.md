# Spool

Spool is a Windows sound logger for long, unattended recordings such as live shows and field sessions. It records one audio input to WAV (BWF/RF64) or Opus files and is built so that a recording keeps going no matter what:

- A capture ring buffer absorbs slow disks.
- Each destination has a chain of folders. If a drive fills up, fails or hangs, recording fails over to the next folder.
- Files are confirmed to disk every few seconds.
- A crashed session resumes when the app restarts.

## Features

- **Destinations:** several at once, each with its own format, channels, folder chain, file rotation (wall-clock aligned) and file-name template.
- **WAV:** 16-, 24- or 32-bit float, with BWF metadata. Files switch to RF64 above 4 GB.
- **Opus:** bitrate is configurable.
- **Markers:**
  - Add a marker now or 5, 10 or 30 s back.
  - Rename markers at any time, during recording or after.
  - Markers are written as WAV cue labels and in each destination's JSON Lines manifest.
- **Problems handled:** a lost input device fills the gap with silence and is picked up again when the device comes back. Low disk space, failovers and gaps all show on the dashboard and in the event log.

## Build

You need Visual Studio 2022 (with the C++ workload) and CMake 3.31 or later. The Opus libraries are downloaded during the build.

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release
```

The output is `build/bin/Release/spool.exe` (the app) and `spool_tests.exe` (the tests).

To try Spool without an audio interface, run it with a test tone:

```bash
build/bin/Release/spool.exe --synthetic
```

## Use

1. Open **Settings…** and choose the input and the destinations.
2. Press **Record**.
3. While recording:
   - **M** adds a marker; the arrow on the **Marker** button places one in the past.
   - **S** splits the file.
   - **F2** renames the newest marker. All markers are listed under **Markers…** next to the event list.
4. **Stop** always asks for confirmation, so a stray click or keystroke can't end a recording. **Ctrl+Alt+M** is an optional global marker hotkey, which you can turn on in Settings.

## Where things are kept

- Recordings and their `.jsonl` manifests go to the folders you configure. The default is `Music\Spool`.
- Settings, the crash state and the window position are in `%APPDATA%\Spool`.
- The event logs are in `%LOCALAPPDATA%\Spool\logs`.
