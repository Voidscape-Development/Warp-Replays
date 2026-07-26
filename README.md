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

### Warp Playlist source

A playlist source in the spirit of the VLC video source, built on the same playback engine as the Warp Media source, so every clip in the list gets the same replay controls:

* **Playlist** of local media files, played in order or shuffled. Shuffled playlists are reshuffled on every pass.
* **Automatic advance**: the next file starts as the current one is about to end. The transition is started one transition-duration early, so it finishes at the moment the current file runs out instead of freezing on its last frame. The next file is opened and parked on its first frame ahead of time, so the incoming half of the transition has picture from the start.
* **Loop the playlist** when the last file ends.
* **Transition between videos**, picked from every transition registered in OBS (Cut, Fade, Swipe, Slide, Fade to Color, Luma Wipe, and any transition plugins installed), with a configurable duration.
* **Playback speed and frame stepping** for the file that is playing, with the same hotkeys as the Warp Media source. The speed resets to the configured Speed value whenever the playlist moves to another file.
* **Hotkeys**: Next Video, Previous Video, Back to First Video, Restart Current Video, Clear Playlist, plus Play/Pause, Stop, the speed hotkeys, and the frame stepping hotkeys.

Clear Playlist empties the source's file list for good — it is written to the scene collection like any other settings change. The cleared file paths are written to the OBS log first, so a playlist cleared by a mis-hit during a show can be rebuilt from there.

Media controls (the ones in the OBS media controls dock) apply to the playlist: restart starts the playlist over from its first file, next and previous move through the list, and the time and duration shown are those of the file that is playing.

### Warp Detection filter

A filter that watches a Warp Media or Warp Playlist source and triggers something else when the operator changes playback. Add it under Filters on any source — it never touches the picture, so the source it is on does not have to be the source it listens to.

* **Listen To**: the Warp Media or Warp Playlist source to watch. Left unset, the filter watches the source it is on. A source that is not in the scene collection yet is picked up as soon as it appears, so the filter survives a scene collection loading in whatever order it likes.
* **React To**, one event per filter. Every speed and frame stepping hotkey has an event of its own, named after that hotkey — *Set Speed to 25%*, *Set Speed to 50%*, *Reset Speed (100%)*, *Set Speed to 125/150/200%*, *Speed Up (+10%)*, *Slow Down (-10%)*, *Step Forward 1/5/10/20 Frames* and *Step Backward 1/5/10/20 Frames* — so every step level can set off something different. Three more events match a value you choose instead:
  * *Speed Set to a Value…*, matching one speed, or any speed.
  * *Frames Skipped Forward…* / *Frames Skipped Backward…*, matching one frame count, or any count.

  Speed being set to a value means it was put there outright: a preset speed hotkey, Reset Speed, or the Speed property. Reaching that same speed by stepping up or down fires the *Speed Up* / *Slow Down* event instead.

  The event list is built from the same tables the sources register their hotkeys from, so a preset speed or step size added to the sources turns up here as well.
* **Then Trigger**:
  * *A Global Hotkey*: any of OBS's own hotkeys — Start Recording, Save Replay, and the rest — listed by the same translated names the Settings → Hotkeys page uses.
  * *A Source Hotkey*: pick a source or scene, then one of its hotkeys, again by the name the hotkeys page shows. Another Warp source's speed and stepping hotkeys are in that list, so one source can drive another.
  * *A Filter*: pick a source and one of its filters, then enable, disable or toggle it — or trigger one of the filter's own hotkeys.

Hotkeys are triggered through OBS's hotkey routing on the UI thread, exactly as if the key had been pressed, and every trigger is written to the OBS log. For several reactions to the same source, add several Warp Detection filters.

An action that drives the source its own filter is listening to would trigger itself forever, so a filter ignores events its own action caused, and stops triggering for the rest of the second after 20 triggers.

Both Warp sources emit the events as signals on their signal handler, so scripts can listen for them too:

```
warp_speed_changed(ptr source, int speed, int prev_speed, string change)
warp_frames_stepped(ptr source, int frames)
```

`change` is `set`, `increased` or `decreased`; `frames` is negative when stepping backward. The speed a file starts at is not a change: a playlist moving to its next file resets the speed without emitting anything.

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
