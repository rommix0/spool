# Sound Logger: Design

Date: 2026-10-16
Status: draft for review
Builds on: `HANDOFF.md` (core ring/sink architecture). Where this document and the handoff disagree, this document wins. Note: the handoff's "switch output while recording" becomes, in v1, failover, rotation, and appending folders to a chain (§10.5). Destinations themselves are fixed once recording starts.

Markers in this document:
- **Decided**: the user chose it.
- **My call**: chosen by the designer (Claude) as a sensible default. The user may still change it. The builder should treat it as the spec unless told otherwise.

---

## 1. Intent

A Windows program that records audio to files for long stretches, from a few hours to a whole day: a live performance, a festival stage, or an unattended field recording session. Once recording starts, audio must keep landing on disk no matter what happens to the disks, the input device, or the program itself, and the person running it must be able to see at a glance that everything is fine.

The core idea, carried over from the handoff, is one continuous capture **ring buffer** that separates capture from storage. Each output reads from the ring independently, so a slow or failing disk never disturbs capture, and outputs can be swapped, rotated, or failed over mid-recording with no gap.

The TTS-corpus use case mentioned in the handoff is **out of scope** (decided). Design for long continuous runs, not per-utterance takes.

## 2. Goals, non-goals, anti-goals

### Goals
- Record continuously for many hours with zero dropped samples under normal conditions.
- Survive a failing, full, or hung output drive by failing over to the next folder with no lost or duplicated samples.
- Survive the input device disappearing (unplugged USB interface) by marking a gap and resuming once the device returns, in the same session.
- Leave playable, recoverable files after a crash or power cut. At most a few seconds may be lost.
- Show the state of the whole session in one glanceable window.
- Write two formats: **WAV (Broadcast WAV)** and **Opus** (decided).
- Minimal dependencies: a native **Win32 GUI** (decided). The only third-party libraries are `libopus` and `libopusenc`.

### Non-goals (v1)
- FLAC, MP3, AAC, or any FFmpeg-based format (decided: no FFmpeg add-on).
- Network streaming output.
- Monitor/headphone output.
- Auto-splitting on silence or onset.
- A "merge segments" command (my call: deferred; the manifest records everything needed to add it later).
- Input gain control. Gain is set on the hardware.
- Editing, playback, or waveform display of finished files.
- Platforms other than Windows 10/11 x64.

### Anti-goals (failure even if it technically works)
- A dropout or gap that nobody was told about. Every gap must be recorded in the manifest and shown in the UI.
- One sick output (slow disk, hung network share) affecting capture or any other output.
- A single stray click or keypress stopping a recording.
- Silent processing by Windows (resampling, auto gain, noise suppression) altering the captured audio.
- A file that can't be opened after a crash.

## 3. Constraints

- **Language/API**: C++ with WASAPI (decided). My call: C++20, MSVC, CMake.
- **GUI**: plain Win32 (decided). Custom controls are drawn with GDI, double-buffered. Direct2D is allowed if GDI meters prove too slow, since it ships with Windows.
- **Dependencies**: `libopus`, `libopusenc`. Nothing else outside the Windows SDK. How they are obtained (vcpkg or vendored source) is left to the builder.
- **Limits**: up to 8 input channels and up to 192 kHz (my call).
- **Ring default**: 30 s, configurable, rounded up to a power-of-two frame count (see handoff §4 for sizing math). 192 kHz × 8 ch × float32 × 30 s ≈ 184 MB, so the ring size must be configurable and the memory pre-faulted at start.

## 4. Architecture

This follows the handoff (§2–§5). It is summarized here with the changes that came out of the design conversation.

### 4.1 Threads and data flow

```
WASAPI capture thread ──► capture ring (SPSC per reader cursor, float32 interleaved)
   (MMCSS "Pro Audio")         │
                               ├──► Destination 1 writer thread ──► WAV/Opus file in folder chain
                               ├──► Destination 2 writer thread ──► ...
                               └──► Meter/analysis reader (UI-rate peak/RMS, clip counts)

UI thread ──► command queue (sample-index timestamped) ──► Session manager
Session manager: owns destinations, rotation, failover, watchdog, manifest, event log
```

- **Capture thread**: MMCSS "Pro Audio" priority. It only converts the device's sample format to float32 and writes it into the ring. It never allocates, locks, or does file I/O.
- **Ring storage**: always float32 interleaved, regardless of device format (my call). Float32 holds 24-bit integer samples exactly, so no precision is lost.
- **Monotonic sample counter**: one 64-bit sample index for the whole session, and it keeps counting through device gaps (see §5.3).
- **Readers**: each destination and the meter reader has its own cursor. The ring is single-producer, multi-consumer, where each consumer behaves like its own SPSC reader. The producer must know the slowest cursor to detect overrun.
- **Command queue**: SPSC from the UI thread, carrying sample-index timestamps (handoff §2).

### 4.2 Destinations

A **destination** is one output stream (my call, and the main model change from the handoff). It has:

- A **format**: WAV or Opus, with format options (§6).
- A **channel map**: which input channels it records, in which order.
- An **ordered folder chain**: primary, secondary, and optionally more.
- A **rotation** policy and a **name template** (§7).

There can be one or more destinations, each running independently: for example, an 8-channel 24-bit WAV to drives D:→E: and a stereo Opus copy to drive F:. Failover happens within a destination's own chain and keeps that destination's format.

### 4.3 Session manager state machine (per destination)

States: `Idle → Preflight → Writing → FailingOver → Writing (next folder) … → LastResort`.

- `Writing`: normal operation.
- `FailingOver`: triggered by a write error, the free-space low-water mark, or the stall watchdog (§8.2). The failed writer reports its last **confirmed** sample (§8.1). A new file opens in the next folder with its cursor at confirmed+1. The old file gets a best-effort finalize on a separate thread and never blocks.
- `LastResort`: no folder left. The destination stops writing, a gap starts in the manifest, and the UI raises a red alarm. The capture and other destinations continue. My call: every 30 s, retry any folder whose health check passes, starting at its current sample. The retry resumes from the live position; data already lost is not recovered. A folder appended to the chain while recording (§10.5) is tried at once.
- **No flapping**: after failover, stay on the new folder until the next rotation boundary. At a rotation boundary, return to the highest-priority healthy folder (my call; the handoff said "next take", and rotation boundaries are the takes here).

## 5. Audio input

### 5.1 Device and mode
- Pick a capture endpoint by device ID, and remember it in settings.
- **Default mode: shared mode with `AUDCLNT_STREAMOPTIONS_RAW`** so Windows applies no audio processing (my call). Exclusive mode is an option.
- **Sample rate is a capture setting** (my call). In exclusive mode the user picks from rates the device supports. In shared/raw mode the rate is the device's configured format, which the UI shows read-only along with a hint to change it in Windows sound settings. The program never resamples for WAV. Opus resamples internally (§6.2).
- **Channels**: the device's channel count, up to 8. Destinations choose subsets.
- Event-driven capture (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`).
- Report WASAPI's `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` as a gap event (possible lost input samples).

### 5.2 Power and priority
- While recording, call `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)` so the system never sleeps. Display sleep is allowed.
- Capture thread at MMCSS "Pro Audio" priority. Writer threads at above-normal priority.

### 5.3 Device loss and recovery
- On `AUDCLNT_E_DEVICE_INVALIDATED`, or a device-removed notification (`IMMNotificationClient`): record a **gap start** with sample index and wall-clock time, keep the session and destinations open, and show a red "Input lost" banner.
- Watch for the same device ID to return, then re-open it. If the format is unchanged, resume into the same files and write a **gap end**. The sample counter advances by the gap's length in wall-clock time × rate, so file time stays aligned with real time. The gap itself is written as **silence** into WAV files, so file duration matches wall time (my call). For Opus, encode silence over the gap too.
- If the device returns with a different sample rate or channel count: **end the session and automatically start a new, linked session** with the new format (my call). Each session's sample index then always means one clock. The new session's first manifest line names the previous session. Destinations whose channel map no longer fits the device are disabled in the new session and shown red.
- Very long gaps: the silence fill is capped at the rotation period. If the device has been gone longer than one rotation period, close the files at the cap and open new files when the device returns (my call).

## 6. Formats

### 6.1 WAV: Broadcast WAV (decided)
- Bit depth: **16-bit int (default), 24-bit int, 32-bit float** (decided: user-selectable, 16-bit default). Converting 16-bit from a higher-resolution source applies TPDF dither (my call). 24-bit and 32-bit float conversions are exact.
- Sample rate: the capture rate (§5.1).
- **`bext` chunk**: OriginatorReference, OriginationDate/Time (wall clock at the file's first sample), TimeReference (samples since local midnight at the first sample), and the program name and version.
- **RF64**: write a `JUNK` placeholder chunk the size of `ds64` at the start of every file, so a file that passes 4 GB can be upgraded in place to RF64 (standard EBU approach).
- **Markers**: `cue ` chunk plus `LIST/adtl` `labl` labels, written at finalize. Markers are also in the manifest, so a crashed file loses only its embedded cues, not the markers.
- **Header updates**: RIFF/data sizes are rewritten each time the data is flushed (§8.1), so a crashed file's header is at most one flush interval stale.

### 6.2 Opus
- Container: Ogg Opus via `libopusenc`. A truncated file plays up to the last complete page.
- **Bitrate**: user-selectable (decided), 32–256 kbps for stereo, VBR. My call: default 128 kbps stereo or 64 kbps mono.
- Output is always 48 kHz (Opus requirement). `libopusenc` resamples from the capture rate.
- **Channels: 1 or 2 per Opus destination** (my call). Multichannel Opus (mapping family 255) plays poorly in common players. To log more channels to Opus, add more destinations (for example channels 1–2 and 3–4).
- Markers: manifest only (my call). The Opus header is written when the file opens, before markers exist, so it is never rewritten.
- Flush policy: request a page flush at the same interval as WAV flushes (§8.1) so data reaches disk promptly.

## 7. Rotation and naming

- **Rotation by time**: default every 30 minutes, user-configurable, or off (my call). Rotation is sample-accurate and gapless: the next file starts at the exact sample after the previous one ends.
- **Rotation is aligned to the wall clock** (my call): with 30-minute rotation, files break at :00 and :30, so the first file of a session is usually shorter. This makes "what happened at 15:12" easy to find.
- **Manual split**: the "Split file" button rotates every destination at the current sample. There is no retroactive split (my call). Writers confirm data to disk every 2 s, so cutting in the past would mean truncating flushed data, which Ogg Opus can't do. A retroactive *marker* (§10.1) covers the need to note "it started 10 s ago".
- Rotation also happens automatically on a format change after device recovery (§5.3).
- **Name template** tokens (my call): `{session}`, `{dest}`, `{date}` (YYYY-MM-DD), `{time}` (HHMMSS of the first sample), `{n}` (file index, zero-padded to 3 digits), `{seg}` (failover segment letter, empty for the first). Default: `{session}_{date}_{time}_{n}{seg}`. Files are never overwritten. On a name collision, append `_1`, `_2`, and so on.
- The session name defaults to `session` and is editable before recording starts.

## 8. Reliability

### 8.1 Confirmed means on disk
- Each writer flushes its data to disk (`FlushFileBuffers`, or the Opus page flush followed by `FlushFileBuffers`) every **2 s** (my call, configurable), then updates the header (WAV), then marks everything written so far as **confirmed**.
- Failover always restarts from the last confirmed sample, so audio that was written but not flushed is written again in the new file rather than trusted.

### 8.2 Failure triggers
1. A write, flush, or open error.
2. Free space below a **low-water mark**: by default, whichever is larger of 10 minutes of audio at the destination's data rate and 1 GB (my call). Checked every 10 s.
3. The **stall watchdog**: if a destination's cursor falls behind the capture position by more than **50% of the ring** (my call, configurable), it's declared failed. A hung write call must never block the session manager or any other destination. Abandoning (leaking) a stuck writer thread is acceptable. The builder chooses the mechanism, for example overlapped I/O with `CancelIoEx` plus thread abandonment.

### 8.3 Preflight (at record start)
- Every folder in every chain: exists or can be created, a test file can be written and deleted, free space is reported.
- Warn (not block) if a fallback folder is on the same physical volume as an earlier folder in its chain.
- Warn if the estimated recording time on the primary is under 1 hour.
- Block start only if no destination has a single writable folder.

### 8.4 Overrun (last resort)
If a destination can't keep up and has nowhere to go, its cursor is moved forward to stay within the ring, and the skipped range is recorded as a gap in its manifest. Capture and other destinations are unaffected. The UI gap counter increments and the destination's panel turns red.

### 8.5 Crash recovery and auto-start
- Settings include "recording in progress" state. On launch after an unclean exit, scan every folder of every destination for files listed in their manifests as open, repair their WAV headers from the actual file length (Opus needs no repair), close the manifest entries, and log what was repaired.
- **Automatic restart after a crash or hang** (my call): register with `RegisterApplicationRestart` (built into Windows, no supervisor process). If the relaunch finds a session that was recording, it repairs the files as above and **resumes the same session**: the sample index advances by the downtime × rate, a `gap_start`/`gap_end` pair covers the downtime, new files open with `{n}` continuing, and the manifests continue. The UI shows an amber "Recovered from crash" notice until acknowledged.
- Optional setting: **start recording on launch** (default off), for unattended setups started from Task Scheduler or the Startup folder.

### 8.6 Time stamps
- Every manifest event records both the sample index and wall-clock time (UTC plus local offset).
- A **clock-pair** event is written every 5 minutes (sample index plus wall clock) so drift between the interface clock and the PC clock can be measured or corrected later.

## 9. Manifest and event log

- **Per-destination manifest**: a JSON Lines file (`<session>_<dest>.jsonl`), one event per line, appended and flushed at each confirmation point. Stored in the folder the destination is currently writing to. After failover, a new manifest continues in the new folder, and its first line names the previous manifest's path.
- Event types (my call): `session_start`, `file_open`, `file_close` (with sample range and byte length), `marker`, `gap_start`, `gap_end`, `failover`, `rotation`, `clock_pair`, `warning`, `session_end`. Every event has `sample`, `utc`, and `local_offset`.
- **App event log**: a human-readable text log of all destinations' events plus app-level events, in `%LOCALAPPDATA%\SoundLogger\logs\`, one file per session. This is what the UI event list shows.

## 10. UI

Layout: **Option A, the dashboard** (decided). One main window:

### 10.1 Top bar
- **Record** / **Stop**. Stop asks for confirmation ("Stop recording?"). Closing the window while recording also asks. Record arms the preflight first (§8.3).
- Large elapsed-time clock (HH:MM:SS) and a line with the device name, rate, channel count, and mode.
- **Marker**: drops a marker at the current sample, with an auto-numbered name. An optional dropdown places it N seconds ago (5, 10, or 30 s, within the ring). The name can be edited afterward in the event list.
- **Split file**: manual rotation (§7).

### 10.2 Levels panel
- One vertical or horizontal meter per input channel: peak with a 2 s hold, plus RMS. Scale from −60 dBFS to 0 dBFS. Clip indication at ≥ −0.1 dBFS.
- A **session clip counter** per channel, reset only by a new session or a click.
- One status line: ring fill %, gap count, and dropped-sample count (should stay at 0).

### 10.3 Destinations panel
- One block per destination: format summary (for example "WAV 24-bit · ch 1–8" or "Opus 128k · ch 1–2"), the current file name, and the folder chain with each folder's state (active / standby / failed) and free space as **hours remaining** at that destination's data rate.
- On failover, the block turns amber and stays amber until the user clicks "Acknowledge". LastResort is red.

### 10.4 Event list
- Scrolling list of events from the app event log, newest first, with time stamps.

### 10.5 Settings dialog
- Input: device, mode (raw shared / exclusive), sample rate (exclusive mode only), ring length.
- Destinations: add, edit, and remove destinations. Each has format, bit depth (WAV) or bitrate (Opus), channel map, folder chain (add, remove, reorder), rotation period, and name template.
- General: session name, flush interval, low-water mark, start-on-launch, marker hotkey.
- Settings are stored in an INI file in `%APPDATA%\SoundLogger\` (my call; Win32 has `GetPrivateProfileString` and needs no JSON library).
- **While recording** (decided): the set of destinations is fixed once recording starts. The only live edits are **appending a folder to an existing destination's chain** (for example, a fresh USB drive) and renaming markers. A newly added folder gets a preflight (§8.3) before it joins the chain. If the destination is in `LastResort`, it switches to the new folder immediately (§4.3). Otherwise the folder waits as the last standby. Folders can't be removed or reordered while recording.

### 10.6 Keyboard
- In-window shortcuts: `M` marker, `S` split. Stop has no single-key shortcut, so it can't be pressed by accident.
- Optional **global** marker hotkey via `RegisterHotKey` (default `Ctrl+Alt+M`, off by default), so markers can be dropped while another app has focus (my call).

## 11. Testing

- **Fault-injection writer**: a test double for the file layer that can be told to error, hang, slow down, or report a full disk at a chosen point. Used to test every failover and watchdog path without real hardware.
- **Synthetic input source**: a test double for the capture side (sine sweep, noise, sample-counter ramp) so the pipeline can run without a device. A sample-counter ramp lets tests verify there are no lost or duplicated samples across rotations and failovers by checking the written data.
- **Device-loss simulation** via the synthetic source (stop and resume, format change).
- **WAV validity**: files open in a standard reader. `bext`, `cue `, and RF64 upgrade are verified.
- **Soak test**: 8+ hours with real hardware and rotation every 5 minutes. Check zero gaps and a stable memory footprint.

## 12. Left to the builder
- Project layout, module boundaries, class names, build scripts, and how `libopus`/`libopusenc` are obtained.
- Exact lock-free ring implementation and memory ordering details.
- Mechanism for abandoning hung I/O (§8.2), within the stated requirement.
- GDI drawing details and the exact control layout and sizes within the dashboard.
- Thread priorities other than the capture thread's.
- Internal error types and logging framework (keep it in-house; no new dependencies).

## 13. Changes from HANDOFF.md
- TTS use case: out of scope.
- Output formats fixed to WAV (BWF) and Opus. FFmpeg dropped. FLAC, network, and monitor sinks out of v1.
- "Destination = format + channel map + folder chain" model added.
- New: input device loss and recovery, stall watchdog, flush-before-confirm, sleep prevention, raw mode, BWF time stamps and clock pairs, crash recovery on launch, auto-start, rotation by time.
- "Merge segments" command deferred.
- GUI is plain Win32 (handoff left this open).
