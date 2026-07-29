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

All hotkeys are per-source and are bound in OBS under Settings → Hotkeys. Every one of these actions can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

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

Every playlist action can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

### Warp Detection filter

A filter that watches a Warp Media or Warp Playlist source and triggers something else when the operator changes playback. Add it under Filters on any source — it never touches the picture, so the source it is on does not have to be the source it listens to.

* **Listen To**: the Warp Media or Warp Playlist source to watch. Left unset, the filter watches the source it is on. A source that is not in the scene collection yet is picked up as soon as it appears, so the filter survives a scene collection loading in whatever order it likes.
* **React To**, one event per filter. Every speed and frame stepping hotkey has an event of its own, named after that hotkey — *Set Speed to 25%*, *Set Speed to 50%*, *Reset Speed (100%)*, *Set Speed to 125/150/200%*, *Speed Up (+10%)*, *Slow Down (-10%)*, *Step Forward 1/5/10/20 Frames* and *Step Backward 1/5/10/20 Frames* — so every step level can set off something different. Three more events match a value you choose instead:
  * *Speed Set to a Value…*, matching one speed, or any speed.
  * *Frames Skipped Forward…* / *Frames Skipped Backward…*, matching one frame count, or any count.

  Ticking *React to any value* on those three hides the value, since there is no longer a value to match.

  Speed being set to a value means it was put there outright: a preset speed hotkey, Reset Speed, or the Speed property. Reaching that same speed by stepping up or down fires the *Speed Up* / *Slow Down* event instead.

  Two events cover the speed moving in a direction whichever way it was asked to:
  * *Speed Increase*, for a speed up press and for a speed set above the one being played at — a faster preset, Reset Speed from below 100%, or the Speed property moved up.
  * *Speed Decrease*, the same the other way.

  Three cover the playback commands themselves, whether they came from the media controls, a hotkey or the websocket:
  * *Media Play*, when playback is started or resumed. On a playlist that has ended or been stopped, pressing play replays it, and counts here.
  * *Media Pause*, when playback is paused.
  * *Media Restart*, when the file or playlist is restarted from the top — the media controls' restart button, the Warp Media *Restart* hotkey, the playlist's *Restart Current Video*, and the `Restart` and `RestartCurrent` websocket requests. Moving through a playlist with next, previous or *Back to First Video* is not a restart.

  These three are about commands, so they only fire for playback someone asked for. Playback the source drives by itself does not report one: a Warp Media source restarting as it goes on screen is not a *Media Restart*, and the pause a frame step does before it steps is not a *Media Pause*. Nor is a playlist reaching its next file a *Media Play*.

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
warp_media_action(ptr source, string action)
```

`change` is `set`, `increased` or `decreased`; `frames` is negative when stepping backward; `action` is `play`, `pause` or `restart`. The speed a file starts at is not a change: a playlist moving to its next file resets the speed without emitting anything. `warp_media_action` reports commands, so playback a source drives by itself — restarting as it goes on screen, the pause a frame step does first, a playlist rolling on to its next file — emits nothing.

### obs-websocket control

Every action of the Warp Media and Warp Playlist sources is also an [obs-websocket](https://github.com/obsproject/obs-websocket) vendor request, so replay playback can be driven from a Stream Deck, a Companion button, a tablet, or any other client outside OBS Studio. The requests are registered when the plugin loads, if obs-websocket is installed; nothing has to be turned on for them.

Clients send them with obs-websocket's `CallVendorRequest`, under the vendor name `warp`:

```json
{
  "vendorName": "warp",
  "requestType": "SpeedUp",
  "requestData": { "sourceName": "Replay" }
}
```

Every request names the source it applies to, with `sourceName` or `sourceUuid`.

| Request | Fields | What it does |
| --- | --- | --- |
| `Play` | | Plays, or resumes a paused source |
| `Pause` | | Pauses |
| `TogglePlayPause` | | Pauses when playing, plays otherwise |
| `Stop` | | Stops |
| `Restart` | | Media: plays the file from the top. Playlist: starts the playlist over from its first file, like the restart button in the media controls |
| `SetCursor` | `cursor` | Seeks to a position in the file, in milliseconds |
| `SetSpeed` | `speed` | Plays at `speed` percent (10 to 400) |
| `SpeedUp` | `amount` | Speeds up, by 10 percentage points unless `amount` says otherwise |
| `SlowDown` | `amount` | Slows down, by 10 percentage points unless `amount` says otherwise |
| `ResetSpeed` | | Back to 100% |
| `StepForward` | `frames` | Steps forward, by 1 frame unless `frames` says otherwise |
| `StepBackward` | `frames` | Steps backward |
| `GetStatus` | | Changes nothing, and answers with where playback stands |

Five more apply to a Warp Playlist source, and answer with an error when they are sent to a Warp Media source:

| Request | What it does |
| --- | --- |
| `Next` | Next video |
| `Previous` | Previous video |
| `First` | Back to the first video |
| `RestartCurrent` | Restarts the video that is playing |
| `ClearPlaylist` | Clears the playlist, exactly as the hotkey does — the file list is emptied for good, and written to the log first |

Every response says whether the request was carried out, and reports where playback stands afterwards, so a control surface can follow the source without asking again:

```json
{
  "success": true,
  "sourceName": "Replay",
  "sourceUuid": "…",
  "sourceKind": "warp_media_source",
  "mediaState": "OBS_MEDIA_STATE_PLAYING",
  "cursor": 4200,
  "duration": 30000,
  "speed": 110
}
```

A Warp Playlist source also reports `playlistIndex` (-1 when nothing is playing), `playlistLength` and `currentFile`. A request that could not be carried out — no such source, a source that is not a Warp source, or a value outside the range the action takes — answers with `"success": false` and an `error` saying what was wrong, and changes nothing.

The requests do exactly what the matching hotkeys do, including emitting the signals above, so a Warp Detection filter reacts to a speed change driven over the websocket the same way it reacts to the hotkey. The difference is that a hotkey only applies to a source that is on screen, because the operator is pressing it at whatever is in front of them, while a request names the source it means and is carried out whether or not it is being shown. `Restart` on a Warp Media source is the exception: the source only restarts playback while it is being shown, over the websocket as from the hotkey.

The playlist actions that have no counterpart in OBS's media control API are proc handlers on the source, so scripts can call them too, and so can anything else that can reach the source:

```
warp_set_speed(int speed)          warp_playlist_first()
warp_adjust_speed(int delta)       warp_playlist_restart_current()
warp_get_speed(out int speed)      warp_playlist_clear()
warp_step_frames(int frames)       warp_playlist_status(out int index, out int count, out string current_file)
```

The first four are on the Warp Media source as well. Play, pause, stop, restart, next, previous and seeking are OBS's own media controls on both sources, so obs-websocket's built-in `TriggerMediaInputAction`, `SetMediaInputCursor` and `GetMediaInputStatus` requests work on them too.

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

GNU General Public License v2.0 (or later). `src/obs-websocket-api.h` is the header [obs-websocket](https://github.com/obsproject/obs-websocket) ships for plugins that add vendor requests, taken as-is (GPL v2.0 or later). The `src/media-playback` directory is adapted from [obs-studio](https://github.com/obsproject/obs-studio)'s `deps/media-playback` library (see `src/media-playback/LICENSE`), extended with on-the-fly speed changes and frame stepping. `src/warp-source.c` is adapted from obs-studio's Media Source (`plugins/obs-ffmpeg/obs-ffmpeg-source.c`).
