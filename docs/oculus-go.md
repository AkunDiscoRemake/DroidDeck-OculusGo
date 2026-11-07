# Oculus Go (experimental)

The Oculus Go has its own APK, `DroidDeck-OculusGo-<version>.apk` (package
`com.droiddeck.launcher.oculusgo`, so it installs beside the standard app). It is built from the
same sources as the `oculusgo` build type. Nothing in this build has been run on a Go yet: it
compiles, packages and passes the host checks below, and headset operation still needs testing.

| | Oculus Go |
|---|---|
| SoC | Snapdragon 821, 4 Kryo cores, Adreno 530 |
| Memory | 3 GB |
| System | Android 7.1.2 (API 25), arm64 kernel 3.18 |
| Display | 2560x1440 LCD, 60/72 Hz, 3DoF head tracking, one 3DoF remote |
| VR runtime | VrApi only (no OpenXR) |

## Building

```sh
DROIDDECK_BUILD_VARIANT=oculusgo tools/build_local.sh
```

or, with runtime inputs already prepared, `./gradlew assembleOculusgo`. Immersive SteamVR needs
the Oculus Mobile SDK, which is not redistributed here. Download it from Meta, accept its licence,
and point the build at it in `local.properties`:

```properties
droiddeck.ovrSdk=/path/to/ovr_sdk_mobile
```

(or `-PovrSdkDir=...`). Use an SDK release that still lists Oculus Go support; the runtime calls
only VrApi functions that Go-era SDKs have. Without the SDK the APK still builds, with the panel
app only. `build_local.sh` then runs `tools/oculusgo/check_api_level.py`, which checks every packaged
native library against the API 25 system libraries.

Install with `adb install DroidDeck-OculusGo-<version>.apk` after enabling developer mode in the
Oculus app. It appears under Library > Unknown Sources.

## Android 7.1 port

The standard APK keeps `minSdk 26`; only the `oculusgo` build type lowers it to 25.

- **Native code.** Below API 26 the NDK build declares unavailable system functions weak
  (`__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__`) and rejects unguarded uses
  (`-Werror=unguarded-availability`). AHardwareBuffer calls go through `cpp/compat/ahb_compat.h`,
  whose failures every caller already treats as "no shared buffers", so the compositor keeps its
  copy paths. On API 26+ builds the checks fold away.
- **Java libraries.** Core library desugaring (`desugar_jdk_libs_nio`) supplies `java.nio.file` and
  `java.time`, which most of the app uses.
- **Android APIs.** `ProcessCompat` covers `Process.waitFor(timeout)`, `isAlive`,
  `destroyForcibly` and output redirection; `Notifications.builder` covers notification
  channels; picture-in-picture, pin requests, launch displays, pointer capture, xattrs and
  amplitude vibration are skipped where the system lacks them. `lint` (`NewApi`) for the
  `oculusgo` variant reports no API 26 call left outside a guard.
- **Prebuilt helpers.** PulseAudio and the Vulkan bridge server reference `nl_langinfo`,
  `strtod_l` and `mallopt`, which bionic gained in 8.0. `libddlegacylibc` supplies them through
  `LD_PRELOAD` (`core/LegacyLibc`). The bridge's in-process JNI server cannot be preloaded, fails
  to load, and the existing fallback starts it as a separate process.
- **Audio.** DirectAudio and the AAudio sinks need AAudio (API 26). Before 8.0 PulseAudio uses its
  `module-pipe-sink`, and `LegacyAudioOut` plays the FIFO through `AudioTrack` (48 kHz stereo,
  about 40 ms of pipe buffer). The DirectAudio relay, which also carries the microphone, needs
  AAudio and is not started: games use PulseAudio and there is no microphone on the Go.

## Graphics: no Turnip, no Venus

Turnip has no Adreno 5xx support, and Venus needs Vulkan 1.2 from the phone's driver while the
Go's Adreno driver reports Vulkan 1.0. On a Go, `DeviceSupport.adreno()` is false and the session
takes the Vulkan bridge (`external/MaliBridge`): the guest's Vulkan calls are replayed on the Go's
own driver, which the bridge's emulation layers extend towards what DXVK, gamescope and SteamVR
expect. The defaults are written once (`oculusGoDefaultsV1`): bridge on, performance mode, Venus off.

If the bridge's self-test fails, `droiddeck-session` switches the session to Mesa's lavapipe
(software Vulkan) and llvmpipe (software GL) instead of a PanVK pair, with Steam's compositing in
software. lavapipe runs on the CPU: expect a usable desktop and Steam UI, and low frame rates in
3D. `LP_NUM_THREADS=3` leaves one core for Android and the VR compositor.

## The VR room: SteamVR only

The Go APK has no 2D screens. The Go shows a 2D activity of a VR app unwarped across both lenses,
and its remote cannot drag a touch UI, so the app is declared `vr_only` and its only launcher entry
is `OculusGoVrActivity`, which opens straight into a VR room (`cpp/govr/go_home.h`): a floor grid,
a panel, and Steam's screen as a flat 16:9 screen in front of you. The remote's laser points;
the trigger or a touchpad click presses.

1. **Install runtime** (first start only) downloads the Linux runtime with the panel's progress bar.
2. The Steam session starts by itself. Its screen is the compositor's output, drawn through a
   `SurfaceTexture` in the room; pointing at it and holding the trigger touches it, so Steam's sign-in
   (the QR code works with the Steam mobile app) and dragging work. Back hides or shows the screen.
3. SteamVR is installed by itself the first time.
4. **Enter SteamVR** switches the room to the immersive path below. Quitting SteamVR from its
   dashboard returns to the room; leaving the app stops the session.

## Immersive SteamVR

Inside SteamVR the guest side is the Quest path's driver, tracking file and frame channel.

- **Head.** VrApi's predicted pose, in the floor-level tracking space when the system offers it
  (otherwise eye level plus 1.6 m). Position is VrApi's head-neck model; poses carry
  `DROIDDECK_XR_POSE_3DOF`, and SteamVR sees "DroidDeck Oculus Go".
- **Remote.** Orientation from VrApi, position from the GVR arm model (`tools/vr/src/arm_model.h`),
  reset on recentering. It keeps the Touch profile, so games' Touch bindings apply:

  | Go remote | SteamVR |
  |---|---|
  | Trigger | trigger (click and full value) |
  | Touchpad position, while touched | joystick |
  | Touchpad click | A |
  | Back (short press) | system button: dashboard |
  | Oculus button | Oculus Home; leaving closes SteamVR |

- **Frames.** The guest's side-by-side image is uploaded once per new frame into a single VrApi
  swapchain; both eye layers point into it through their tan-angle matrices, which also flip the
  rows. Nothing is redrawn on the Go's GPU, and VrApi's time warp reprojects the last image to the
  newest head pose every refresh, with chromatic aberration correction.
- **Pacing.** The display runs at 72 Hz; SteamVR is told half of that (`guestDivisor` 2), so it
  renders 36 images per second and time warp fills the rest. CPU and GPU clock levels are 4
  (maximum) while in VR, the main thread is VrApi's performance thread, and extra-latency mode
  gives the upload a frame of slack.
- **Resolution.** Eyes default to 75 % of VrApi's suggested size (768x768 instead of 1024x1024).
  SteamVR's supersampling is fixed at 1.0 and SteamVR Home is disabled for the Go
  (`droiddeck-steamvr`); its dashboard remains.

Settings live in the `session` preferences:

| Key | Default | Range |
|---|---|---|
| `oculusGo.refreshHz` | 72 | 60 or 72 |
| `oculusGo.guestDivisor` | 2 | 1-4 |
| `oculusGo.renderScale` | 75 | 40-100 (% of the suggested eye size) |
| `oculusGo.cpuLevel` / `oculusGo.gpuLevel` | 4 | 0-4 |

## Expectations

The Go is far below the Quest 2 this pipeline was measured on: four 2016 cores run FEX (Steam's
x86 client), Proton and SteamVR, inside 3 GB of memory. Even with the bridge working, treat
SteamVR as a dashboard and light-scene experiment; with the lavapipe fallback it is a few frames
per second. The 3.18 kernel lacks some newer system calls that the runtime may use.

## Diagnostics

- `files/oculusgo-startup.txt`: model, Android version, GPU, chosen Vulkan path and VR settings.
- `Android/data/com.droiddeck.launcher.oculusgo/files/go-vr-health.json`: VR mode, refresh rate,
  frames received/uploaded, remote hand, SteamVR state, last error.
- Session logs: `== vkbridge:` lines show the bridge self-test and any lavapipe fallback;
  `audio.log` shows the AudioTrack sink.
- Logcat tags `DroidDeckGoVR`, `OculusGo`, `LegacyAudioOut`.
