# LibrePods for Windows

Use your AirPods on Windows — battery status, noise control (ANC /
transparency / adaptive), ear detection, head tracking, stem gestures,
hearing-aid tuning, and system-tray integration.

This is a Windows port of [LibrePods](https://github.com/kavishdevar/librepods)
(Android / Linux) by Kavish Devar. All credit for the protocol research,
packet layouts, and original implementation goes to the upstream project.
`windows/shared/` contains shared sources carried over from the Linux tree.

## Layout

- `windows/` — the Qt6/QML application (`windows/main.cpp`, CMake build)
- `windows/svc/` — user-mode service bridging the app to the driver
- `windows/driver/kmdf/` — KMDF profile driver exposing an L2CAP channel
  (Windows has no user-mode L2CAP API)
- `windows/shared/` — platform-independent protocol/UI code shared with
  the Linux port (packet parsing, ATT manager, QML components, assets)

## Building

See `windows/README.md`. Requires Qt 6 (Quick/Bluetooth), CMake, and a
C++20 toolchain. The KMDF driver additionally requires WDK + EWDK and a
test-signing configuration — see `windows/driver/kmdf/README.md`.

## License

GPL-3.0, same as upstream. See `LICENSE`.
