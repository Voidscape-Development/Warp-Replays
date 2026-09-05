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
* **Zoom and framing** of the clip that is loaded — see [zoom](#zoom). The framing goes back to the whole picture as each clip lands, so an [instant replay](#instant-replays) arrives ready to go on screen rather than still zoomed into the last one.

All hotkeys are per-source and are bound in OBS under Settings → Hotkeys. Every one of these actions can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

### Warp Playlist source

A playlist source in the spirit of the VLC video source, built on the same playback engine as the Warp Media source, so every clip in the list gets the same replay controls:

* **Playlist** of local media files, played in order or shuffled. Shuffled playlists are reshuffled on every pass.
* **Automatic advance**: the next file starts as the current one is about to end. An overlapping transition is started early enough that it finishes at the moment the current file runs out instead of freezing on its last frame. The next file is opened and parked on its first frame ahead of time, so the incoming half of the transition has picture from the start.
* **Loop the playlist** when the last file ends.
* **Transition between videos**, picked from every transition registered in OBS (Cut, Fade, Swipe, Slide, Fade to Color, Luma Wipe, Stinger, and any transition plugins installed), configured in the Transition group of the source settings:
  * **Transition Properties**, which opens the settings of the transition itself — direction, color, the stinger video file, anything the transition that is picked has to offer — in the same window OBS opens for the transitions in its own list, preview and all. They are saved with the playlist rather than with the transition, so each playlist configures its transitions its own way, and picking a transition back up brings its settings with it. A stinger's transition point is read from there too: the playlist moves on that far before the current file ends, so the swap behind the stinger still lands on the last frame of the outgoing file, and a stinger video that has gone missing is reported to the OBS missing files dialog along with the playlist's own files.
  * **Duration**, and **timing**: whether the transition overlaps the end of the file, so the file plays out in full underneath it, or runs once the file has played to its last frame.
  * **Scaling** and **alignment**, for playlists whose files are not all the same size.
  * **Use a different transition when going back**, which gives moving back through the playlist a transition of its own — a second section with its own transition, properties, duration, scaling and alignment. The first section is then the one the playlist goes forward with: automatic advance, Next Video and starting the playlist over. With the option off, the one transition handles both directions. Both transitions are kept live and set up separately, so a stinger for going back keeps its own video loaded and is ready the moment it is asked for, and the picture is handed from one to the other as the direction changes rather than blinking out in between. Going back has no timing setting of its own: only automatic advance is placed against the end of a file, and that only ever moves forward.
* **Playback speed and frame stepping** for the file that is playing, with the same hotkeys as the Warp Media source. The speed resets to the configured Speed value whenever the playlist moves to another file.
* **Zoom and framing** for the video that is playing — see [zoom](#zoom) — with the same rule the speed follows: the framing belongs to the video, not to the playlist. Zoom into a corner of one replay and the playlist moves on to the next file at the whole picture, ready to go, however far in the last one was left. The file being transitioned away from keeps the framing it was being watched at, so it plays out the transition as it was on screen while the incoming one is already framed at the top.
* **Audio in the OBS mixer.** The playlist hands OBS the sound of the file that is playing as its own, the way any other source does, so it sits in the audio mixer with a fader, a mute, monitoring, audio filters and its own track routing. The files themselves are played only through the playlist, so the fader is the last word on how loud it is. A transition crossfades the two files it is holding, and a stinger brings its own sound with it.
* **Volume**, from silent to 100% of the audio in the files themselves, and **Transition Volume**, the same for the sound a stinger brings with it, which appears alongside it while a stinger is picked. They are set before the playlist reaches the mixer, so the fader, mute, monitoring and audio filters all still apply on top of them. Both apply live, to the file that is playing as well as to the ones after it, so a playlist of clips recorded far louder than the rest of the show can be turned down without touching the files — and because the two levels are separate, a playlist can be run silent while its stinger still sounds.
* **Hotkeys**: Next Video, Previous Video, Back to First Video, Restart Current Video, Clear Playlist, plus Play/Pause, Stop, the speed hotkeys, and the frame stepping hotkeys.

Clear Playlist empties the source's file list for good — it is written to the scene collection like any other settings change. The cleared file paths are written to the OBS log first, so a playlist cleared by a mis-hit during a show can be rebuilt from there. It is the one playlist hotkey that is not limited to a source that is on screen: the others drive playback, so they are aimed at what the operator has in front of them, while this one edits the file list, which is most often emptied while the playlist is off air.

Media controls (the ones in the OBS media controls dock) apply to the playlist: restart starts the playlist over from its first file, next and previous move through the list, and the time and duration shown are those of the file that is playing.

Every playlist action can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

### Zoom

Every Warp source can be zoomed into and panned around while it plays, and so can any other source, through the **Warp Zoom** filter. It is one system in three places: the filter that draws it, the presets a framing is kept as, and the dock an operator works it from.

The picture is magnified inside the frame the source already fills. Nothing downstream has to know a zoom is on: the source reports the size it always did, so a playlist's transitions, the scene item it sits in and the canvas are all untouched, and a zoomed source can be faded, moved or stingered exactly as before.

* **Zoom** from 100% to 800%. 100% is the whole picture and the zoom never goes below it, so there is no way to end up with the frame smaller than the video.
* **Pan** anywhere inside the picture. The window is kept inside the video, so the edge of the video stays at the edge of the frame however far it is pushed: panning right at 400% stops the moment the right-hand edge of the file is at the right-hand edge of the screen.
* **Presets**: a framing kept under a name, recalled with a smooth move. Every source has as many as it needs, plus a **Default** preset that is always the first in the list, is always the whole picture, and cannot be renamed, moved, overwritten or removed — whatever else is set up, there is always one way back.
* **Moves glide** rather than jumping. A preset recall eases in and out over 400ms by default, and each preset can be given a glide of its own, so a wide establishing shot can ease in slowly while a tight punch-in snaps across. The zoom and pan hotkeys use a shorter glide, short enough that a held-down key runs its presses into one continuous move.
* **Confirm framing**, off by default: with it on, nothing reframes the source on the spot. The dock, the hotkeys and the websocket line the shot up instead, and it goes to air when it is taken — see [lining a shot up](#lining-a-shot-up).
* **Hotkeys**, per source, in Settings → Hotkeys: Zoom In, Zoom Out, Reset Zoom, Pan Left / Right / Up / Down and the four corners, Take and Drop Staged Framing, *Recall Zoom Preset 1* through *8*, and one hotkey per preset named after it. The numbered slots fire whichever preset is in that position, so their bindings survive a preset being renamed or removed mid-show; a preset's own hotkey follows it through a rename, because it is bound to the preset rather than to its name.
* The **Zoom** group in the source properties sets how long moves take, and, on the Warp Zoom filter, the framing itself.

Zoom In and Zoom Out multiply by 1.25 rather than adding a fixed amount, so a press feels the same at 600% as it does at 150%. Panning moves by a twentieth of what is on screen, which covers the same distance on the stream whatever the zoom is. The four corner directions move a full step on both axes at once, so pushing into a corner covers more ground than a straight press, the way a joystick held into its corner does.

On the Warp sources the framing belongs to the **video**, not to the source: a playlist moving to its next file, and a clip landing in a Warp Media source, both put it back to the whole picture without a word, the same way the speed goes back to what the source is set to. Nothing about the framing is saved with those sources — the presets are, the framing is not, because it was that video's. The Warp Zoom filter is the other way round: dropped on a camera or a scene, it keeps the framing it was left with, like any other filter.

#### Lining a shot up

**Confirm framing** turns the zoom into a preview bus. With it on, nothing an operator does reframes the source on the spot: the shot is lined up first, and only goes to air when it is taken. It is a per-source setting, saved with the scene collection and set from either the dock or the source properties, so a replay feed can run on confirm while a camera stays direct.

Everything that would reframe the source stages instead — dragging the picture, the pad, the zoom slider, a preset recall, Reset, the hotkeys and the websocket requests — so a hotkey behaves the way the dock says it will rather than the two disagreeing.

While a shot is waiting:

* The dock's picture shows **the shot you are building** rather than what is on air, so you judge the framing by looking at it. The only pixels anyone has are the ones the source is putting out, so a shot lined up inside the live one — which is what tightening in from a wider shot always is, including the whole-picture start every clip gets — is exact. A shot that reaches outside it cannot be drawn from what is there, and those parts are hatched rather than invented.
* The corner map shows **both windows**: what is on air in white, and the shot waiting in amber.
* **Take** puts it up, easing into it the way a preset recall does. **Cancel** drops it. Both have hotkeys of their own, so a shot lined up in the dock can be taken from a Stream Deck, and both are websocket requests.

A shot that is never taken is dropped when the video changes — a playlist reaching its next file, or a clip landing in a Warp Media source — since it was composed against a video that is no longer playing. Turning confirm off drops it too: a framing goes to air because somebody took it, never because a setting was switched.

Sources emit `warp_zoom_staged` as a shot is lined up, taken or dropped, so a control surface can follow it. A shot going to air is a `warp_zoom_changed` like any other, which is what a [Warp Detection filter](#warp-detection-filter) reacts to — it fires when the picture actually moves, not when the shot was lined up.

#### The Warp Zoom dock

A dock called **Warp Zoom** is added to OBS when the plugin loads — turn it on under Docks if it is not showing. It is the panel a framing is found from, laid out like a PTZ desk:

* The **picture** the chosen source is putting out, framed as it is right now. **Drag it to pan** — the picture follows the cursor, so it is moved rather than aimed — and **roll the wheel to zoom** in and out around it. A dragged pan tracks the hand exactly rather than gliding behind it; everything else in the dock eases. A small map in the corner says where the framing sits in the whole picture once there is anything left out.
* A **zoom slider**, and an eight-way pad — the four straight directions and the four corners — around a dot that resets the framing, with zoom in and out standing beside it as their own column. The pad takes the room the dock has: its buttons are square and grow with the dock's width, so a wide dock gets a pad worth hitting without looking, and a dock narrowed to a strip beside the picture still gets one worth aiming at. They stay square by taking whichever of the width and the height there is less room for, so a pad in a dock with no height to spare comes down to buttons that fit rather than standing its rows apart, and a pad with the room takes the dock's width across, with the rows a hair apart rather than a margin down either side. What keeps it from crowding out the rest is the share of the dock's height it is allowed rather than any one size, so the picture and the presets keep their room.
* **Confirm framing**, with **Take** and **Cancel** appearing beside it while a shot is waiting.
* The **presets** of that source, standing together on a panel a shade under the dock behind them — the list, the buttons that work it and Path read as the one section rather than as more rows in a column. The shade is mixed from the OBS theme that is on, so it follows a dark theme and a light one alike. Double-click a preset, or press Recall, to move to it. **Save** stores the framing that is on screen right now under a name you type, the way a PTZ desk stores a shot; **Update** moves a preset that already exists to the framing on screen, keeping its name and its hotkey; Rename and Remove do what they say, and none of them are offered for the Default preset. The number in front of a preset is the numbered recall hotkey that fires it. **Path** says how the picture travels to the preset that is picked: *Direct* holds the shot in its place in the frame the whole way across, so the move lands without a swing, and *Arc* walks it across the picture at a flat rate instead, which throws it out to the side of the frame as the move starts and lets it creep back as the zoom catches up.
* The **speed** the source is playing at, with the same preset speeds the hotkeys use — framing and speed being the two things ridden live.

Right-click the dock for **Full** or **Minimal**. Minimal drops the picture and keeps everything else — the source list, the zoom, the pad, the presets and the speed — for an operator who frames from presets and the pad and would rather have the room. With the picture gone the pad is what the panel is, so it is let grow further than it does beside one. Nothing is rendered while it is minimal, so the dock costs only what its widgets cost. The choice is saved with your OBS user settings rather than with the scene collection, since it is how you like to work rather than anything about the show.

The dock is pointed at one source with the list at the top, or set to **follow the active source**, in which case it frames whichever Warp source, or source carrying a Warp Zoom filter, is on program right now. Leave it off to set a framing on a feed before it goes on air.

The dock shortens as well as narrows. The picture is what gives the height up: it asks for a comfortable size and hands all of it back as the dock is dragged shorter, and below the point where it would be a strip too thin to frame a shot in it is put away altogether — the zoom, the pad, the presets and the speed keep working in a dock too short for a picture, rather than the picture being laid out underneath them. Nothing is rendered while it is away, the same as in Minimal, and it comes back the moment there is room for it again.

The dock narrows to about a button's width. The rows of buttons — the preset ones, the preset speeds, and Take and Cancel — take the next line down rather than holding the panel open, and the source list and the longer labels say as much as there is room for, with the whole of it in the tooltip. Nothing has to be given up to fit the dock on a small screen or in a narrow column beside the preview.

Presets can also be set up by number, which is what the **Zoom Presets…** button in the [Warp window](#warp-flows) opens: every source that can be zoomed, its presets, and the zoom, centre, glide and path of each one written out.

#### The Warp Zoom filter

**Warp Zoom** is in the OBS filter list, so anything can be framed with it — a camera, a browser source, a whole scene. It carries everything the Warp sources' own zoom does: its own presets, its own hotkeys, and its own entry in the dock, listed as *source - filter*. Its framing and presets are saved with the filter, so it is left the way it was found.

The Warp sources put a driven zoom filter on what they are showing themselves — the playlist on every file it opens, the media source on itself the first time anything asks it to zoom. Those are not operators' filters: they render the framing the source that made them hands over, and they are what keeps the zoom with the video rather than with the source.

### Warp Detection filter

A filter that watches a Warp Media or Warp Playlist source and triggers something else when the operator changes playback. Add it under Filters on any source — it never touches the picture, so the source it is on does not have to be the source it listens to.

* **Listen To**: the Warp Media or Warp Playlist source to watch, or any source carrying a Warp Zoom filter, whose framing is watched through that filter. Left unset, the filter watches the source it is on. A source that is not in the scene collection yet is picked up as soon as it appears, so the filter survives a scene collection loading in whatever order it likes.
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

  Three are about how the source is framed rather than about playback, and fire for a zoom driven from anywhere — a hotkey, the dock, the websocket:
  * *Zoom Changed*, whenever the framing moves at all.
  * *Zoom Preset Recalled…*, matching one preset by name, or any of them.
  * *Zoom Reset*, when the framing goes back to the whole picture.

  The framing a video starts at is not a change: a playlist reaching its next file resets the zoom without firing anything, the same way it resets the speed.

  One more is about the file rather than about playback:
  * *Clip Loaded*, when a clip is put in a Warp Media source from outside it — which is what an [Instant Replay flow](#instant-replays) does as each replay is saved. This is the event to react to for bringing an instant replay on screen: point it at a hotkey, or at a filter that runs your slide-in.

  The first three are about commands, so they only fire for playback someone asked for. Playback the source drives by itself does not report one: a Warp Media source restarting as it goes on screen is not a *Media Restart*, and the pause a frame step does before it steps is not a *Media Pause*. Nor is a playlist reaching its next file a *Media Play*.

  The event list is built from the same tables the sources register their hotkeys from, so a preset speed or step size added to the sources turns up here as well.
* **Then Trigger**:
  * *A Global Hotkey*: any of OBS's own hotkeys — Start Recording, Save Replay, and the rest — listed by the same translated names the Settings → Hotkeys page uses.
  * *A Source Hotkey*: pick a source or scene, then one of its hotkeys, again by the name the hotkeys page shows. Another Warp source's speed and stepping hotkeys are in that list, so one source can drive another.
  * *A Filter*: pick a source and one of its filters, then enable, disable or toggle it — or trigger one of the filter's own hotkeys.
  * *A Zoom Preset*: pick a source that can be zoomed and one of its presets, and the source is framed with it. This is what auto-framing hangs on — an [Instant Replay flow](#instant-replays) loading a clip can frame it before it goes on screen, by reacting to *Clip Loaded* with the preset the replay should arrive at.

Hotkeys are triggered through OBS's hotkey routing on the UI thread, exactly as if the key had been pressed, and every trigger is written to the OBS log. For several reactions to the same source, add several Warp Detection filters.

An action that drives the source its own filter is listening to would trigger itself forever, so a filter ignores events its own action caused, and stops triggering for the rest of the second after 20 triggers.

Both Warp sources emit the events as signals on their signal handler, so scripts can listen for them too:

```
warp_speed_changed(ptr source, int speed, int prev_speed, string change)
warp_frames_stepped(ptr source, int frames)
warp_media_action(ptr source, string action)
warp_zoom_changed(ptr source, float zoom, float x, float y, string change, string preset)
warp_zoom_staged(ptr source, bool staged, float zoom, float x, float y, string change, string preset)
```

On `warp_zoom_changed`, `zoom` is 1 for the whole picture and up to 8 at the tightest, `x` and `y` are where the middle of what is shown sits in the file, and `change` is `manual`, `preset`, `reset` or `set`, with `preset` naming the preset when one was recalled. A Warp Zoom filter emits it on itself. `warp_zoom_staged` reports a shot being [lined up](#lining-a-shot-up), and again with `staged` false as it is taken or dropped; nothing has moved on screen when it fires.

For the rest, `change` is `set`, `increased` or `decreased`; `frames` is negative when stepping backward; `action` is `play`, `pause` or `restart`. The speed a file starts at is not a change: a playlist moving to its next file resets the speed without emitting anything. `warp_media_action` reports commands, so playback a source drives by itself — restarting as it goes on screen, the pause a frame step does first, a playlist rolling on to its next file — emits nothing.

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

Nine more frame the source rather than driving playback, and are the one group that is not limited to the Warp sources: they work on anything that can be zoomed, which is a Warp source or any source carrying a [Warp Zoom filter](#the-warp-zoom-filter). Add `filterName` to name which filter to frame with when a source carries more than one; without it the source's own zoom is used, or the one filter it has.

| Request | Fields | What it does |
| --- | --- | --- |
| `SetZoom` | `zoom`, `x`, `y`, `glide` | Frames the source. `zoom` is a percentage from 100 to 800, `x` and `y` are where the middle of the picture sits, in percent. Leave a field out and it stays where it is, so a control surface can pan without knowing the zoom |
| `ZoomIn` | `factor`, `glide` | Zooms in, multiplying by 1.25 unless `factor` says otherwise |
| `ZoomOut` | `factor`, `glide` | Zooms out the same way |
| `PanZoom` | `dx`, `dy`, `glide` | Pans by a percentage of what is on screen |
| `ResetZoom` | `glide` | Back to the whole picture |
| `RecallZoomPreset` | `preset`, `slot` | Moves to a preset, named by `preset` (its name or its id) or numbered by `slot` |
| `SaveZoomPreset` | `name` | Keeps the framing the source has right now as a preset, and answers with its `presetId` |
| `RemoveZoomPreset` | `preset` | Removes a preset. The Default preset cannot be removed and answers with an error |
| `GetZoomPresets` | | Changes nothing, and answers with the presets |
| `SetZoomConfirm` | `confirm` | Turns [lining shots up](#lining-a-shot-up) on or off for that source. Turning it off drops whatever was waiting |
| `TakeZoom` | | Puts the shot that is lined up on air. Answers with an error when there is nothing waiting |
| `DropZoom` | | Drops the shot that is lined up, leaving the picture where it is |

`glide` is how long the move takes, in milliseconds; leave it out for the source's own preset glide, or send `0` for a move that lands at once. Every one of these answers with `zoomPresets`, so a control surface can lay out its buttons from the reply it already has.

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

Every response also reports `zoom`, `zoomX` and `zoomY` — how the source is framed, in percent — along with `zoomFilter` when the framing lives in a filter rather than in the source itself, so a zoom can be followed without asking for it separately. `zoomConfirm` says whether that source lines shots up, and `zoomStaged` whether one is waiting; when one is, `zoomStagedZoom`, `zoomStagedX` and `zoomStagedY` are the shot it would take. A Warp Playlist source also reports `playlistIndex` (-1 when nothing is playing), `playlistLength` and `currentFile`. A request that could not be carried out — no such source, a source that is not a Warp source, or a value outside the range the action takes — answers with `"success": false` and an `error` saying what was wrong, and changes nothing.

The requests do exactly what the matching hotkeys do, including emitting the signals above, so a Warp Detection filter reacts to a speed change driven over the websocket the same way it reacts to the hotkey. The difference is that a hotkey only applies to a source that is on screen, because the operator is pressing it at whatever is in front of them, while a request names the source it means and is carried out whether or not it is being shown. There are two exceptions, one either way. `Restart` on a Warp Media source only restarts playback while the source is being shown, over the websocket as from the hotkey. `ClearPlaylist` edits the file list rather than driving playback, so its hotkey is carried out whether or not the playlist is on screen, the same as the request.

Three more work on a [Warp flow](#warp-flows) rather than on a source, and name it with `flowName` (or `flowId`) instead of `sourceName`:

| Request | What it does |
| --- | --- |
| `SaveToFlow` | Saves the replay buffer and keeps the clip for that flow, exactly as its Save Replay hotkey does. Answers with an error when the replay buffer is not running |
| `AddLastReplayToFlow` | Puts the replay that was saved last in that flow, as its Add Last Saved Clip hotkey does |
| `GetFlows` | Changes nothing, and answers with every flow in the scene collection |

They answer with the flow they applied to, and how much is in the playlist it feeds:

```json
{
  "success": true,
  "flowId": "flow_…",
  "flowName": "Match Replays",
  "flowKind": "replay",
  "targetName": "Replay Feed",
  "clipCount": 4
}
```

`GetFlows` answers with `flows`, an array of those same objects, and takes no fields.

The playlist actions that have no counterpart in OBS's media control API are proc handlers on the source, so scripts can call them too, and so can anything else that can reach the source:

```
warp_set_speed(int speed)          warp_playlist_first()
warp_adjust_speed(int delta)       warp_playlist_restart_current()
warp_get_speed(out int speed)      warp_playlist_clear()
warp_step_frames(int frames)       warp_playlist_status(out int index, out int count, out string current_file)
```

Everything that can be zoomed carries the framing procs as well, which is how the dock, the websocket requests and a Warp Detection filter all drive it without caring which kind of source they are holding:

```
warp_zoom_set(float zoom, float x, float y, int glide)
warp_zoom_get(out float zoom, out float x, out float y, out float target_zoom, out float target_x, out float target_y)
warp_zoom_adjust(float factor, int glide)          warp_zoom_save_preset(string name, out string id)
warp_zoom_pan(float dx, float dy, int glide)       warp_zoom_update_preset(string id, string name, float zoom, float x, float y, int glide, out bool found)
warp_zoom_reset(int glide)                         warp_zoom_remove_preset(string id, out bool removed)
warp_zoom_recall(string preset, int slot, out bool found)  warp_zoom_move_preset(string id, int delta, out bool moved)
warp_zoom_presets(out string presets)
warp_zoom_confirm(bool confirm, out bool state)    warp_zoom_take(out bool taken)
warp_zoom_drop(out bool dropped)
```

These take the zoom as a factor from 1 to 8 and the position from 0 to 1, rather than the percentages the websocket requests and the properties use. `warp_zoom_presets` answers with the presets as JSON, since a calldata cannot carry an array.

The first four playlist procs are on the Warp Media source as well, along with `warp_media_load(string path, int speed, string playback)`, which points the source at a file and says what playback does with it — `keep`, `play` or `hold`, as the [Instant Replay flow](#instant-replays) settings describe. It is what an instant replay flow calls, and it emits the source's `loaded` media action. Play, pause, stop, restart, next, previous and seeking are OBS's own media controls on both sources, so obs-websocket's built-in `TriggerMediaInputAction`, `SetMediaInputCursor` and `GetMediaInputStatus` requests work on them too.

### Warp flows

A **Warp** entry in the Tools menu opens the Warp window: the flows of the scene collection, what each one feeds, and how much is in it. Underneath the list, a dot says whether the replay buffer is running — green while it is, red while it is not — next to the clip it saved last, and a **Zoom Presets…** button opens the [zoom presets](#the-warp-zoom-dock) of every source that can be framed.

A flow takes the clips the OBS replay buffer saves and hands them to a Warp source, so a feed builds itself as an event goes on. Adding one opens a dialog laid out like OBS's own Add Source: the kinds down the left, what the one that is picked does on the right, and the settings it needs underneath.

* **Replay List** — a list that fills itself with the clips the replay buffer saves.
* **Highlight List** — a list that keeps the clips worth keeping. It works the same way, and is usually fed by a replay list rather than by the replay buffer directly.
* **Combo List** — a replay list and a highlight list, made and linked in one go. The replay list takes the saves and hands every clip it takes to the highlight list as well. Each has its own playlist source and its own order, so the replay feed can run newest first while the highlights stay in the order they happened.
* **Instant Replay** — one clip in a Warp Media source, loaded the moment it is saved, so the replay that just happened is ready to go on screen over the action. See [instant replays](#instant-replays) below.

Every flow is:

* **Feeds**: the Warp source clips are handed to — a Warp Playlist source for the list kinds, a Warp Media source for an instant replay — picked from the ones already in the scene collection or made on the spot. A new one is put in the current scene, since a source no scene holds on to is not saved with the scene collection.
* **Takes**: where its clips come from.
  * *Only what its own hotkey saves* — the flow's own Save Replay hotkey saves the replay buffer and keeps the clip for that flow. Saves made any other way go past it, so two flows with two hotkeys feed two different lists.
  * *Every replay buffer save* — the flow takes those as well as the saves nobody claimed: OBS's own Save Replay hotkey, obs-websocket, a script.

  A flow's Save Replay hotkey works either way. What the setting decides is whether saves the flow did not ask for land in it too.
* **Order**: *oldest first*, added to the end, so the list plays in the order the clips were saved; or *newest first*, added to the top, so the clip that was just saved is the next one up. Not offered for an instant replay, which holds one clip rather than a list.
* **Limit**: drop the oldest clip once the list is longer than a number you set. The oldest is the one that would be played last, whichever way round the list is built. Off by default, and not offered for highlight lists or instant replays: what is in one was put there on purpose, and the other holds a single clip.
* **Also feeds**: the flows this one hands every clip it takes on to, so a highlight reel builds itself alongside a replay feed. Links are followed through further links, and a flow already being fed is not fed twice.
* **Take clips**: a flow that is switched off takes nothing itself, but still passes clips on to the flows it is linked to.

Each flow registers two hotkeys of its own, named after it in Settings → Hotkeys:

* **Warp: Save Replay to *flow*** — saves the replay buffer and keeps the clip for that flow. Pressed while the replay buffer is not running, it says so and offers to start it there and then.
* **Warp: Add Last Saved Clip to *flow*** — puts the replay that was saved last in that flow without saving a new one, for a clip worth keeping that nobody knew about in advance.

The keys they are bound to are saved with the flow, so a flow loads with its hotkey the way it was left.

A clip landing in a playlist disturbs nothing about playback: the file list is edited the way the playlist's own Clear Playlist edits it, so whatever is on screen plays out untouched and the new clip is reached in its turn. A clip that is already in the list is not added a second time, and a flow whose playlist source has gone says so in the OBS log rather than losing the clip quietly.

Flows are saved with the scene collection, alongside the sources they feed, so an event day's collection carries its own feed setup and switching collections switches flows.

Every flow action can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

#### Instant replays

An **Instant Replay** flow feeds a Warp Media source instead of a playlist. There is no list: the source is pointed at the clip that was just saved, the one before it is let go, and the source is left holding a replay of what happened seconds ago — ready to slide in over the live action, drop into a corner, or fill the screen while play is stopped.

It has two settings of its own on top of the ones every flow has:

* **When a clip lands**:
  * *Load it and leave playback alone* — the source's own settings decide from there. A source that is on screen starts the clip; one that is not waits, and plays it from the top as it is brought on, if *Restart playback when source becomes active* is ticked on the source. This is the one to use for a source that is slid in.
  * *Load it and play it now* — the clip starts from the top there and then, on screen or not. Bring the source on part-way through and you join the replay where it has got to.
  * *Load it and hold the first frame* — the clip is parked on its opening frame and left there until someone plays it, for an operator who wants to call the moment themselves. A source set to restart when it becomes active still restarts as it is brought on, so a clip that is to stay held through the reveal wants that setting off.
* **Speed**: the percentage the clip is played at, for a replay that comes in slow. Off by default, which leaves the source playing at whatever speed it is set to. The speed hotkeys still work on the source while the clip plays, so an operator can slow it down further mid-replay.

**Bringing it on screen is yours to set up**, which is the point: the flow loads the clip and says so, and how the source arrives is whatever you have built. The source emits its `warp_media_action` signal with `loaded` as each clip lands, so a [Warp Detection filter](#warp-detection-filter) on it, listening for **Clip Loaded**, can trigger a hotkey, switch a filter on, or fire whatever drives your slide-in. A move/scale animation, a scene item toggle, a stinger — the flow does not care which. The signal is emitted as soon as the clip is in rather than once it has decoded, so a source that is only being loaded says so too; *play* and *hold* have the file decoding straight away, so there is picture by the time an animation lands.

The same event can frame the clip as well as reveal it: a second Warp Detection filter listening for **Clip Loaded** with *A Zoom Preset* as its trigger frames the source before it arrives, so a replay can come in already punched in on the corner of the pitch it happened on. The framing goes back to the whole picture by itself as the next clip lands.

An instant replay flow takes clips the same way the list kinds do — from its own **Save Replay to** hotkey, or from every replay buffer save — and it can be fed by another flow instead, so one save can fill a replay list *and* load the instant replay source at once. Its **Add Last Saved Clip to** hotkey reloads the clip that was saved last without saving a new one.

The Warp window shows an instant replay flow holding 0 or 1 clips, and what it does with one where a list shows its order.

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
