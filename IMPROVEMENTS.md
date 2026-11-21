# OpenCamera: analysis and improvements

Branch `improve-pipeline-and-ui`. Everything below is about this branch compared with upstream `dd7805f`.

## How it worked before

```
Phone (Android)                                PC (Windows)
camera (Camera1) ── NV21 preview callbacks ──► Java NV21 convert ─► MediaCodec H.264
                                                    │ RTP over UDP (libstreaming RTSP server)
                                                    ▼
EncomService.exe: FFmpeg 4.3 RTSP client ─► SW decode ─► sws bicubic → RGB24 buffer
   ─► std::reverse_copy into one of 2 shared-memory buffers (Global\, mutex per buffer)
EncomCamera*.dll (DirectShow source, inside Zoom/Chrome/...):
   alternates buffers, spins with Sleep(5)/goto while busy, memcpy → 640x480 RGB24 only
Control: phone connects TCP 65500, sends its IP + RTSP port, then 2 heartbeat bytes per second.
```

## What was wrong, ranked by impact

| # | Problem | Effect |
|---|---------|--------|
| 1 | The PC requested **50 Mbps** (`h264=50000-…`; the value is kbps) for 640x480 | Wi-Fi bursts and UDP loss, then smeared or frozen video and lag |
| 2 | Connection watchdog used a 100 µs `select` timeout and treated "no byte in 100 µs" as "disconnected" | The camera thread stopped seconds after starting; video dropped all the time |
| 3 | FFmpeg defaults: 500 ms reorder buffer, 5 s stream probe, no read timeout after open (`av_read_frame` blocks forever if the phone vanishes), `setup()` also did a fixed `sleep(2000)` | Latency, slow start, hangs and no reconnect |
| 4 | Shared memory: two buffers with named mutexes; the filter alternated buffers strictly, so it could show frames out of order; it busy-waited with `goto`; it ran a whole-system process scan (`CreateToolhelp32Snapshot`) every 30 frames on the streaming thread | Stutter, wasted CPU, and the reader could block the writer |
| 5 | Extra full-frame passes per frame: bicubic scale to RGB24, then `reverse_copy` (also mirrored the image for viewers), then memcpy in the filter | CPU per frame and a mirrored picture |
| 6 | `Global\` objects need **administrator rights**, so the app always asked for UAC | Friction, and running as admin for no reason |
| 7 | The server socket was bound to the IP of the route to 1.1.1.1 | Failed with no internet (phone hotspot, offline LAN) or with several adapters |
| 8 | Virtual camera: fixed 640x480 at 30 fps; `CheckMediaType` only accepted the current type | Low quality; some apps fail to negotiate |
| 9 | Phone: the buffer-mode encoder does a Java NV21 copy and conversion for every frame; `measureFramerate()` blocks 0.7–2 s on every start and never reads its own cache; random RTSP port 1000–2000 can't bind below 1024 (about 2% of tries fail) | CPU and heat on the phone, slow start, random failures |
| 10 | Phone UI: type the PC's IP by hand; a "Scan" button pings 256 hosts with ICMP (slow, often blocked) and prints them to a log. Views were updated from background threads, which crashes. | Hard to use; crashes when the connection fails |
| 11 | PC UI: three fixed panels with logs, Accept/Decline for every connect, no preview, always on top, redraw at 60 fps even when idle | Hard to use for non-technical people |
| 12 | Build: `jcenter()` is gone, so the Android build fails today; the code used deprecated FFmpeg APIs | Couldn't be built as-is |

## What changed

### PC: frame path
- **New shared-memory transport** (`Windows/shared/FrameShm.h`): a lock-free triple buffer with per-slot sequence counters. The writer never waits; readers always get the **newest complete** frame (late frames are dropped, not queued) and need only **read** access. A heartbeat replaces the process scan. It uses `Local\` names, so **no admin is needed**.
- **One conversion, no copies:** FFmpeg's scaler writes BGR24 *bottom-up* straight into the shared slot (negative stride). The bicubic pass, the intermediate RGB buffer and `reverse_copy` are gone. Mirroring is now an optional toggle, done in the copy the filter makes anyway.
- **Decoder** (`VideoReceiver.cpp`): send/receive API (builds with FFmpeg 4.3 *and* 8), low-delay flags, slice threads only (frame threads add latency), 100 ms reorder window, 2 MB UDP socket buffer, short probe, real read timeout through the interrupt callback (instant cancel too). Optional **D3D11VA GPU decoding** with automatic fallback to the CPU.
  - Measured on this RTX PC, CPU decoding is faster end to end (0.66 ms per 720p frame against 3.0 ms on the GPU, because the picture has to come back to system memory for DirectShow), so CPU is the default and GPU is a toggle for weak CPUs.
- **Virtual camera** (`EncomCamera/Filters.cpp`): offers 1280x720 (default), 1920x1080, 640x480 and 640x360; letterbox-scales if the app picks a size that differs from the phone's; paces delivery to new frames (about 1 ms wait without changing the system timer) and repeats the last frame instead of stalling; timestamps from the real clock. It shows a friendly **"Waiting for your phone"** picture instead of black, and the device is named "OpenCamera".

### PC: connection
- Listens on **all adapters**, so it works with a phone hotspot and without internet.
- **Auto-discovery:** answers UDP probes on port 65501 and broadcasts a beacon, so the phone lists the PC by name.
- **Trusted phones:** "Always allow this phone" remembers it in `%APPDATA%\OpenCamera\trusted.txt`, so there's no prompt next time. "Block" lasts until restart.
- **Real watchdog** (6 s without heartbeat) and **automatic video reconnect** while the phone is still connected.
- **Resolution and fps negotiation:** 480p, 720p or 1080p at 30 fps, with sensible bitrates (2, 4 or 8 Mbps) sent to the phone.
- Backward compatible: the original Play Store phone app still connects. The new app also sends its name, and an 'F' byte when you tap "Switch camera" on the phone.

### PC: UI (`Ui.cpp`)
A dark, modern single window: a **live preview** with a stats chip (resolution, fps, Mbps, ms on PC), a pulsing **status pill**, and **3-step onboarding** that shows the PC's name (as the phone lists it) and its addresses as a fallback. It has Front/Back and 480p/720p/1080p switches, toggles for mirror and GPU decoding, an **Allow / Block** dialog, a one-click **"Install virtual camera"** (one UAC prompt registers the 64- and 32-bit filters), and an activity log tucked behind a button. Errors are in plain words. The window is resizable and DPI-aware, it isn't always on top, and it sleeps when idle or minimised.

### Phone
- **Zero-copy encoding:** the camera renders into the encoder's input surface on the GPU (`MODE_MEDIACODEC_API_2`) instead of NV21 copies through Java. The encoder runs at realtime priority, and the frame-rate measurement is cached, which removes up to 2 s from every start.
- **New UI:** a full-screen preview, a status chip, and a bottom card listing **computers found on the Wi-Fi**: tap one to connect. Typing an address is the fallback. It **reconnects automatically** to the last PC and retries with back-off until you tap Disconnect. While connected it shows Switch camera / Disconnect and a "keep this app open" hint. A clear screen handles a missing camera permission.
- No AndroidX: framework widgets only, 1 MB APK. libstreaming is compiled into the app as source (one module).
- Build: AGP 9.5.1, Gradle 9.9, compileSdk/targetSdk 36, minSdk 21. The RTSP port is a stable 20000–29999 and moves if it's taken.

## What was verified, and what wasn't

| Item | Status |
|------|--------|
| `EncomService.exe` (x64) and `EncomCamerax64.dll` / `EncomCamerax86.dll` build with VS 2019 (v142) against the repo's FFmpeg 4.3 | **Built** |
| Android `app-debug.apk` builds (`gradle --offline assembleDebug`) | **Built** |
| Decoder → shared memory → reader, with your FFmpeg 8: 720p/1080p, CPU and GPU, limited and full range, 1440p downscale; checks colour and orientation of every frame | **Tested, all pass** (`Windows/tests`) |
| Lock-free buffer under stress (writer at about 700 fps, 3 readers, about 19k reads) | **Tested, 0 torn frames** |
| Phone link with a fake phone on loopback: discovery reply, name, approval, camera switch from the phone, 6 s watchdog, old-app protocol, disconnect | **Tested, all pass** |
| PC window look and behaviour | **Not run.** `EncomService.exe` contains the repo's prebuilt FFmpeg/SDL2 binaries, which I didn't execute without your OK |
| Virtual camera in Zoom/Chrome/OBS | **Not tested.** It needs registering (`regsvr32`, admin, a system change) |
| Phone app on a real phone, end-to-end latency | **Not tested.** It needs the APK installed on your phone |
| Old upstream phone app against the new PC app | Protocol checked with the fake phone only |

Known limits / follow-ups: RTP is still UDP-only (libstreaming's RTSP server doesn't do TCP interleaving). It still uses the Camera1 API. The phone app must stay in the foreground (Android blocks background camera use). There's no QR pairing yet; discovery covers that need on a normal LAN.

## Building

- **PC:** open `Windows/EncomService/EncomService.sln` (VS 2019+, v142), Release x64; build `EncomCamera` for x64 and Win32. Put `EncomCamerax64.dll`, `EncomCamerax86.dll` and `SDL2.dll` next to `EncomService.exe`, then click "Install virtual camera" in the app.
- **Tests:** `Windows\tests\build_and_run.bat C:\path\to\ffmpeg` (a shared FFmpeg build with `include\` and `lib\`).
- **Phone:** `cd Android && gradle assembleDebug` (or Android Studio). The APK is at `app/build/outputs/apk/debug/`.
