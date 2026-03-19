# Audio Device Tester (openSUSE Leap 16.0)

Minimal desktop GUI to enumerate playback devices and play a short test tone on each one.

## Screenshot

![Audio Device Tester screenshot](./screenshot.png)

## Goals

- Simple GUI for audio output testing.
- Keep dependencies minimal and system-native.
- Produce a single executable (`audio-device-tester`).

## Stack

- Language: C11
- GUI: GTK 3
- Audio API: ALSA
- Build system: CMake

## Build (openSUSE Leap 16.0)

Install build dependencies:

```bash
sudo zypper install -y gcc cmake make pkg-config gtk3-devel alsa-lib-devel
```

Build:

```bash
cmake -S . -B build
cmake --build build -j
```

Run:

```bash
./build/audio-device-tester
```

## Command-line options

```
audio-device-tester [OPTION]
```

| Option | Description |
|--------|-------------|
| *(none)* | Open the GTK GUI. |
| `--cycle` | Silently advance to the next audio output sink and exit. Wraps around after the last sink. Tries PipeWire/Pulse first, falls back to ALSA. Exits 0 on success, 1 on failure. Useful as a hotkey binding. |
| `--help` | Print usage information and exit. |

## Notes

- Device enumeration uses ALSA PCM hints and shows output-capable entries.
- A `Hide technical device names` checkbox is enabled by default, so the list prefers human-readable labels.
- The test button plays a 1-second 880 Hz sine tone.
- PipeWire/Pulse sinks are preferred; ALSA hardware devices are used as a fallback.
- The `--cycle` option is designed for hotkey/script use: it produces no output and exits immediately.
