# Warp

Warp is an OBS Studio plugin for handling replays at video game events and in any other situation that calls for instant-replay control.

## Features

### Warp Media source

A media source based on OBS Studio's built-in Media Source, with the same properties and behavior, plus live playback control designed for replay operation:

* **On-the-fly playback speed** from 10% to 400%, changed without restarting or losing the current position:
  * Speed Up (+10%) / Slow Down (-10%) / Reset Speed (100%) hotkeys
  * Preset speed hotkeys: 25%, 50%, 125%, 150%, 200%
  * The Speed property in the source settings also applies live
* **Frame stepping** hotkeys: step 1, 5, 10, or 20 frames forward or backward. Stepping while the video is playing pauses it first; stepping is frame-accurate, including backward steps.

All hotkeys are per-source and are bound in OBS under Settings → Hotkeys.

### Warp window

A Warp entry in the Tools menu opens the Warp window (currently an empty placeholder for upcoming replay tooling).

## Building

This plugin is based on the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) and builds the same way. See the template's [wiki](https://github.com/obsproject/obs-plugintemplate/wiki) for build system requirements and options.

In addition to the template's requirements, the plugin links against FFmpeg (`avcodec`, `avdevice`, `avformat`, `avutil`, `swscale`). On Linux, install the corresponding `-dev` packages; on Windows and macOS the obs-deps prebuilt dependencies already include FFmpeg.

Quick start on Linux:

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

GitHub Actions workflows build the plugin for Windows, macOS, and Ubuntu on every push and pull request.

## License

GNU General Public License v2.0 (or later). The `src/media-playback` directory is adapted from [obs-studio](https://github.com/obsproject/obs-studio)'s `deps/media-playback` library (see `src/media-playback/LICENSE`), extended with on-the-fly speed changes and frame stepping. `src/warp-source.c` is adapted from obs-studio's Media Source (`plugins/obs-ffmpeg/obs-ffmpeg-source.c`).
