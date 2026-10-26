# LibrePods for Windows

Qt6/QML port of the Linux LibrePods app. Reuses the Linux packet code, parsers,
QML UI, and tray infrastructure; only the platform layers are substituted.

## Status

Working with real AirPods Pro 2 on Windows 10/11 — **Qt 6.8.3 + llvm-mingw 17**
(verified). Live-verified end to end: AACP channel over the KMDF driver,
per-pod battery, both-pods ear detection, all four noise-control modes
(UI + `librepods-ctl`), conversational awareness, hearing aid toggle,
ear-detection media pause/resume, autostart, single-instance `reopen`, and
ad-arbitrated in-case/pair UI. The MSVC path has not been built yet.

## Build prerequisites

- Windows 10 1809+ (WinRT Bluetooth APIs)
- Windows 10 SDK (e.g. 10.0.19041) — provides the WinRT ABI headers and
  `WindowsApp.lib`/`bthprops.lib` import libraries
- Qt 6.5+ — components: `qtbase` (Quick, Widgets, Bluetooth, Network),
  `qtconnectivity`, LinguistTools
- CMake 3.21+
- A C++20 toolchain:
  - **llvm-mingw 17** (verified): `aqt install-tool windows desktop
    tools_llvm_mingw1706 -O C:/Qt/Tools` plus
    `aqt install-qt windows desktop 6.8.3 win64_llvm_mingw -m qtconnectivity`
  - **MSVC 2022** (Desktop C++ workload, untested)
- No OpenSSL needed: `shims/openssl/aes.h` maps the AES calls used by
  `ble/bleutils.cpp` onto Windows CNG (bcrypt)

### C++/WinRT projection headers

The projection shipped inside the Windows SDK predates C++20 (it includes the
removed `<experimental/coroutine>`). Generate a fresh projection with
`cppwinrt.exe` (from the `Microsoft.Windows.CppWinRT` NuGet package) once:

```powershell
cppwinrt.exe -input "C:/Program Files (x86)/Windows Kits/10/UnionMetadata/10.0.19041.0" `
             -output C:/Qt/Tools/cppwinrt-proj
```

CMake looks for it at `C:/Qt/Tools/cppwinrt-proj` by default; override with
`-DLIBREPODS_CPPWINRT_DIR=<path>`. With MSVC the SDK-bundled projection works
and this step is unnecessary.

## Build

### llvm-mingw

```powershell
cd windows
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/Qt/Tools/llvm-mingw1706_64/bin/clang.exe `
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/llvm-mingw1706_64/bin/mingw32-make.exe `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/llvm-mingw_64
cmake --build build -j
```

### MSVC

```powershell
cd windows
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.x/msvc2022_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Deploy the Qt runtime next to the exe:

```powershell
C:/Qt/6.8.3/llvm-mingw_64/bin/windeployqt.exe --qmldir ../linux build/librepods.exe
```

## Platform layer mapping

| Linux (BlueZ/PulseAudio/DBus) | Windows |
|---|---|
| `QBluetoothDeviceDiscoveryAgent` manufacturer data | `ble/blemanager.cpp`: WinRT `BluetoothLEAdvertisementWatcher` (Qt exposes no manufacturer data on Windows) |
| `BluetoothMonitor` (DBus `org.bluez`) | `BluetoothMonitor.cpp`: `DeviceInformation::CreateWatcher` filtered on `BluetoothConnectionStatus.Connected`, AirPods filtered by SDP records |
| `QBluetoothSocket` L2CAP | `driverl2capsocket.*`: IOCTL client of the LibrePods KMDF profile driver (`windows/driver/`), which opens the AACP channel in kernel mode. Falls back to `l2capsocket.*` (Winsock `AF_BTH`) when the driver is not installed — Windows has no user-mode L2CAP, so the driver is required for a working AACP link |
| MPRIS player control | `media/mediacontroller.cpp`: `GlobalSystemMediaTransportControlsSessionManager` |
| PulseAudio volume ducking | `IAudioEndpointVolume` on the default render endpoint |
| `.desktop` autostart | `HKCU\...\CurrentVersion\Run` registry value |
| logind `PrepareForSleep` | `WM_POWERBROADCAST` via `QAbstractNativeEventFilter` |
| `bluetoothctl connect/disconnect` | Not needed — opening the L2CAP channel establishes the link |

Shared with Linux unchanged: `airpods_packets.h`, `enums.h`, `battery.hpp`,
`eardetection.hpp`, `deviceinfo.hpp`, `ble/bleutils.*`, `ble/bleinfo.h`,
`ble/proximityparser.*` (extracted from the Linux `blemanager` for reuse),
`trayiconmanager.*`, `QRCodeImageProvider.hpp`, QR-Code-generator, all QML and
assets, `librepods-ctl.cpp`.

## Usage notes

- Pair AirPods in **Windows Settings → Bluetooth** first; the app takes over
  the AACP control channel from there.
- Install the KMDF driver once so AACP works — build + install instructions
  are in `driver/kmdf/README.md` (`driver-build.cmd bthl2cap`, then
  `install-test-driver.cmd bthl2cap`, which self-elevates and may require a
  reboot for TESTSIGNING). **Test signing cannot load while Secure Boot is
  on**: run `prepare-secureboot-change.ps1` first — it saves the BitLocker
  recovery protectors, suspends BitLocker for 2 reboots, and prints the
  Secure Boot toggle steps (VMware: VM Settings → Boot Options; physical:
  UEFI setup). The app prefers the driver automatically and logs its status
  at startup; without it only detection/UI features work.
- `--debug` enables verbose logging; `--hide` starts minimized to tray.
- `librepods-ctl noise:anc|noise:off|noise:transparency|noise:adaptive|reopen`
  sends commands to the running instance (same local-socket protocol as Linux).
- Settings page: device identity (serials, firmware, hardware revision,
  Bluetooth address) decoded from the AACP information packet, head-tracking
  toggle + live orientation, rename, ear-detection behavior, retry count,
  one-bud ANC, cross-device relay, notifications, autostart, and a
  "Check coverage" link to Apple's warranty checker (coverage data itself is
  only available server-side at Apple — the serials shown are what it needs).
- **Head gestures**: while head tracking is on, the 0x17 stream also feeds a
  port of Android's `GestureDetector` — nod (accept) and shake (reject) get
  configurable actions (none / play-pause / next / previous / volume ± /
  cycle noise mode) from dropdowns in the playback section.
- **Stem press (0x19)**: each press type (single/double/triple/long) maps to
  the same action list. Assigning an action sends STEM_CONFIG (control id
  0x39) so the pods' built-in mapping doesn't double-fire; leave at None to
  keep native behavior.
- **Custom EQ (0x63)**: enable switch + low/mid/high sliders — writes the
  three-band AACP packet.
- **TiPi / multidevice**: the session member list (0x2E) is shown under
  Connectivity; **Take over audio** claims ownership (0x06) and sends the
  smart-routing media-info + hijack packets (0x10) like Android's takeover.
  When the pods report another host as the audio source (0x0E), ownership is
  released automatically instead of fighting the peer.
- **Persistent logging**: every session appends to `librepods.log` next to
  the exe (rotates at 5 MB); unhandled exceptions also write a
  `librepods-crash-*.dmp` minidump — crashes are diagnosable after the fact.
- The disconnect toast only fires after a **sustained** (~8 s) link-down —
  the brief flaps while audio profiles come up during first insertion no
  longer read as "disconnected".

## What works without the driver

Even when the KMDF driver is absent and the AACP channel can't be opened,
the app still shows real data — AirPods continuously broadcast a BLE
"proximity pairing" advertisement whose plaintext fields carry:

| Data | Source |
|---|---|
| Device name + model (icon art) | Windows paired-device record + ad model ID |
| Pod battery levels (10% steps) | Plaintext nibbles in the ad |
| Case battery (10% steps) | Plaintext nibble — only while a pod is in the case or the lid is open |
| Charging flags | Plaintext flags byte |
| Left/right in-ear detection | Plaintext status bits |
| Pods-in-case / lid open-closed | Plaintext status bits |
| Connection state (idle/music/call) | Plaintext byte |

What still needs the driver: the AACP control channel itself — noise-control
switching, ear-detection actions, and the *encrypted* ad tail that yields
finer battery resolution and the magic IRK/ENC keys. Once AACP connects one
time, the keys persist (`QSettings`) and encrypted-ad decoding keeps working
even if the driver is removed later.

## Known limitations

- **A2DP profile switching**: Windows manages audio profiles at the OS level;
  `activateA2dpProfile`/`removeAudioOutputDevice` are no-ops. Hands-free vs.
  stereo selection is done in Windows Sound settings.
- **`isActiveOutputDeviceAirPods`**: heuristics — matches the default render
  endpoint's Bluetooth address property, then falls back to a friendly-name
  substring match.
- **Phone disconnect requests**: closing the L2CAP channel releases control
  but does not drop the audio link the way `bluetoothctl disconnect` does.
- **SDP PSM lookup** in the Winsock shim uses a `BTHNS_RESTRICTIONBLOB` query
  and falls back to the fixed AirPods PSM `0x1001`.
- **ATT channel (PSM 31)**: implemented in driver protocol v2 — the open
  request now carries a `psm` field (0 = AACP). `AttManager` speaks raw ATT
  PDUs (read/write characteristic, CCCD enables, notifications) and feeds a
  `HearingAidModel` (amplification/balance/tone/ambient-NR/conversation-
  boost/own-voice sliders in settings). Requires the **v2 driver** — with a
  v1 driver installed the app logs a skip notice and everything else keeps
  working. The channel opens a beat after AACP and retries once on failure;
  the settings card shows a status row (`opening` / `failed — reason`)
  whenever the characteristic hasn't arrived. Loud-sound-reduction (handle
  0x1B) is read for parity but marked non-functional upstream.

## Runtime notes

- The AACP channel owns pod state while open; BLE ads are ignored so rotating
  advertisers can't fight authoritative telemetry. In degraded mode,
  proximity changes need two consecutive agreeing ads (a both-docked report
  applies immediately) and lid state only updates from advertisers that carry
  it — pod ads report lid as UNKNOWN.
- A 2-second disconnect grace window keeps micro-flaps from tearing down the
  socket, clearing pod state, or spamming notifications; `podsInCase`/lid
  state survive link drops.
- Retry pacing is link-state aware: fast retries only on a live link, a slow
  pull-up cadence while down — continuous page attempts otherwise collide
  with the pods' own inbound page-in.
- Every kernel BRB (open/close/ACL) is submitted with a 15 s
  `WDF_REQUEST_SEND_OPTIONS` timeout — an unresponsive peer can no longer
  wedge the channel-open ioctl in the kernel and produce an unkillable
  zombie process.
- ATT callbacks guard on "is this still the live socket" before touching the
  manager, and teardown severs signals first — a link flap mid-open can no
  longer deliver `connected` to a dead `AttManager`.

## Ear-event popup & channel self-healing

- The ear-event popup fires on real pod moves (ear/hand/case), evaluated per
  pod — a `Disconnected` status on one side of a pod's transition is link
  churn, but a lagging pod must not mask the other's real move. Window hidden
  or backgrounded → the flyout repositions to the tray and flashes for
  `earPopupSecs`; open **and** focused → the card pulses and a tray toast
  carries the event instead of moving the window.
- Case-lid-open surfaces the same popup driven by BLE advertisements alone —
  it fires even while the control channel is wedged.
- The flyout needs `setPersistentSceneGraph` + `setPersistentGraphics`:
  frameless translucent `QQuickWindow`s can come back from `hide()` as
  transparent-but-present HWNDs otherwise (taskbar button exists, nothing
  renders). `WindowStaysOnTopHint` is required because `raise()` cannot beat
  the focused app's window on a non-activated show. Focus-loss auto-dismiss
  keeps topmost safe.
- Channel self-healing: open failures while link-down build a streak that
  lengthens the pull-up backoff (10s → 60s cap) — hammering opens keeps the
  pod-side channel table wedged. A channel that opens but delivers no packet
  within 15s is recycled as a failure. At streak 8 (with a 10min cooldown)
  the WinRT Bluetooth radio is reset — the only reliable un-wedge.
- Dev hook: send `popup` to the `librepods_server` local pipe to exercise
  the exact ear-event path without pods.
