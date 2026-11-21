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

### Pro camera controls (3.1)
Camera-style manual controls on the PC, sent to the phone over the existing control connection (port 65500, text lines; old apps ignore them).

- **Phone camera engine** (`CameraEngine.java`, `Camera2H264Stream.java`): Camera2 renders straight into the encoder surface (and a preview on the phone). It reports what the camera can do (ISO and shutter range, EV, zoom, manual focus, OIS/EIS, frame rates per size) and a status every second (live ISO/shutter/focus, battery, temperature, how the phone is held).
- **Pro strip under the PC preview**: ISO, SHUTTER, EV, WB, FOCUS, ZOOM, AE-L, LIGHT, MORE. Each tile opens a small panel: ISO and shutter stops, EV slider, white balance presets plus a Kelvin slider (2300-10000 K, from the sensor's colour calibration), AF-C/MF with a near/far slider and AF-L, zoom slider, OIS/EIS and anti-flicker. Auto tiles show the live value the phone picked; manual ones turn amber. Setting only ISO or only shutter gives a semi-auto mode, where the PC keeps the exposure right with the other one.
- **Click the picture to focus and meter there**; right-click clears it.
- **Monitoring aids** (toggles on the preview): grid, histogram, zebra (clipped highlights), focus peaking, and a level line.
- **Rotation**: "Rotate" steps 0/90/180/270, "Auto" follows how the phone is held, "Fill 16:9" crops a portrait picture to fill the frame instead of letterboxing. Rotation costs about 3 ms per 1080p frame on the PC.
- **Frame rate**: picks from the rates the phone supports at the chosen size; the bitrate scales with it.
- **Phone settings screen**: show/hide preview (saves battery and heat), video data Saver/Standard/High, tell the PC how the phone is held, dim the screen while live, reconnect automatically, forget the saved PC, and an About section with what the camera supports.
- The PC warns when the phone is warm (42 °C or more) or the battery is low.
- All of these settings are remembered in `%APPDATA%\OpenCamera\settings.ini`.

### Wi-Fi or USB cable
- The PC shows a **"Wi-Fi" or "USB cable"** badge next to the phone's name (from the Windows adapter the connection arrived on; USB tethering appears as a Remote NDIS/NCM adapter). The phone's status chip says the same.
- The phone groups each PC's addresses. With **"Prefer the USB cable"** on (default), it moves to the cable within about 2 s of the PC answering there, and goes back to Wi-Fi if the cable goes away.
- **"Use USB cable instead of Wi-Fi"** on the phone checks the cable is plugged in, then opens Android's tethering screen; turning USB tethering on stays the user's tap.
- **Cable only, no tethering (adb):** when USB debugging is on and `adb.exe` is found (next to OpenCamera, on PATH, or in the Android SDK), the PC runs `adb reverse` for ports 65500/65501 so the phone reaches it at 127.0.0.1 through the cable, and `adb forward` for the phone's RTSP port. Video then runs as RTP inside the RTSP connection (`rtsp_transport=tcp`; the phone's RTSP server now answers `RTP/AVP/TCP;interleaved`). No firewall change is needed (loopback only). Tested live: 30 fps at 4.5 Mbps over the cable; after an adb restart it was live on the cable again within about 10 s, and with the cable path gone it fell back to Wi-Fi within about 10 s.
- adb is started with only its output pipe inherited, so the adb server never holds OpenCamera's ports.
- Windows names the tethering network "Unidentified network" and treats it as **Public**. OpenCamera's firewall rule on this PC only allows **Private** networks, so the phone stays on Wi-Fi until that network is set to Private in Windows settings.

### Own identity (so it can sit beside the original)
- Phone app ID is now `io.github.zunzelf.opencamera` (was `com.cns.encom`, the original's Play Store ID). It installs as a separate app.
- The virtual camera is called **"OpenCamera Z"** with its own ID `{191F6B23-B6B0-4897-A5C8-F735F36E265E}` (the original, and the sample it came from, used `{8E14549A-...}`). Registering it no longer replaces another app's camera.
- Kept for compatibility: ports 65500/65501 and the control protocol, so the original phone app still works with this PC app.

## What was verified, and what wasn't

| Item | Status |
|------|--------|
| `EncomService.exe` (x64) and `EncomCamerax64.dll` / `EncomCamerax86.dll` build with VS 2019 (v142) against the repo's FFmpeg 4.3 | **Built** |
| Android `app-debug.apk` builds (`gradle --offline assembleDebug`) | **Built** |
| Decoder → shared memory → reader, with your FFmpeg 8: 720p/1080p, CPU and GPU, limited and full range, 1440p downscale; checks colour and orientation of every frame | **Tested, all pass** (`Windows/tests`) |
| Lock-free buffer under stress (writer at about 700 fps, 3 readers, about 19k reads) | **Tested, 0 torn frames** |
| Phone link with a fake phone on loopback: discovery reply, name, approval, camera switch from the phone, 6 s watchdog, old-app protocol, disconnect | **Tested, all pass** |
| Phone app on POCO X3 Pro (Android 13), Camera2 path, 720p | **Tested live:** 27-30 fps, preview upright, settings screen shows |
| Pro controls end to end: ISO 200, 1/50 s, 3200 K, 2x zoom applied on the phone; semi-auto (fixed shutter, auto ISO) exposes well; live ISO/shutter/focus/battery/temperature readouts | **Tested live** |
| Rotation 90 deg, grid, histogram, zebra, peaking overlays | **Tested live** |
| Virtual camera registered and captured from FFmpeg (dshow) | **Tested** |
| Pro tile popups, click-to-focus, level line (phone was lying flat), Fill crop in the virtual camera, 60 fps | **Not tested** (popups need a real mouse on the window; the phone only offered 30 fps at 720p) |
| PC window look and behaviour | **Not run.** `EncomService.exe` contains the repo's prebuilt FFmpeg/SDL2 binaries, which I didn't execute without your OK |
| Virtual camera in Zoom/Chrome/OBS | **Not tested.** It needs registering (`regsvr32`, admin, a system change) |
| Phone app on a real phone, end-to-end latency | **Not tested.** It needs the APK installed on your phone |
| Old upstream phone app against the new PC app | Protocol checked with the fake phone only |

Known limits / follow-ups: RTP is still UDP-only (libstreaming's RTSP server doesn't do TCP interleaving). The phone app now uses Camera2 (libstreaming's Camera1 path stays as a fallback). The phone app must stay in the foreground (Android blocks background camera use). There's no QR pairing yet; discovery covers that need on a normal LAN.

## Building

- **PC:** open `Windows/EncomService/EncomService.sln` (VS 2019+, v142), Release x64; build `EncomCamera` for x64 and Win32. Put `EncomCamerax64.dll`, `EncomCamerax86.dll` and `SDL2.dll` next to `EncomService.exe`, then click "Install virtual camera" in the app.
- **Tests:** `Windows\tests\build_and_run.bat C:\path\to\ffmpeg` (a shared FFmpeg build with `include\` and `lib\`).
- **Phone:** `cd Android && gradle assembleDebug` (or Android Studio). The APK is at `app/build/outputs/apk/debug/`.
