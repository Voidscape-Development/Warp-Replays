# Warp

Warp is an OBS Studio plugin for handling replays at video game events and in any other situation that calls for instant-replay control.

## Features

### Warp Media source

A media source based on OBS Studio's built-in Media Source, with the same properties and behavior, plus live playback control designed for replay operation:

* **On-the-fly playback speed** from 10% to 400%, changed without restarting or losing the current position. The sound keeps its pitch at any speed, so a slow-motion replay sounds slowed down rather than deeper; turn *Keep pitch at other speeds* off in the source settings for the old effect, where the pitch moves with the speed:
  * Speed Up (+10%) / Slow Down (-10%) / Reset Speed (100%) hotkeys
  * Preset speed hotkeys: 25%, 50%, 125%, 150%, 200%
  * The Speed property in the source settings also applies live
* **Frame stepping** hotkeys: step 1, 5, 10, or 20 frames forward or backward. Stepping while the video is playing pauses it first; stepping is frame-accurate, including backward steps.
* **Clips of a file**: a clip loaded into the source can be played from an in point to an out point, or as the last so many seconds of its file. The cut is frame-accurate and nothing is written to disk — the source plays that stretch of the file as if it were the whole of it, and the media controls show the clip's own position and length. This is how an [instant replay flow](#instant-replays) keeps the last few seconds of a save, and it is open to scripts and obs-websocket through `LoadClip`.
* **Seamless reloads**: loading a new clip keeps the picture that is up on screen until the new clip has a frame to replace it with, so a clip landing in a source that is on air cuts from one picture to the next rather than flashing black. The source keeps its decoder from one clip to the next, so a clip loads in a few milliseconds.
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
  * **Use a different transition when going back**, asked just above the transition sections, which gives moving back through the playlist a transition of its own — a second section with its own transition, properties, duration, scaling and alignment. The first section is then the one the playlist goes forward with: automatic advance, Next Video and starting the playlist over. With the option off, the one transition handles both directions. Both transitions are kept live and set up separately, so a stinger for going back keeps its own video loaded and is ready the moment it is asked for, and the picture is handed from one to the other as the direction changes rather than blinking out in between. Going back has no timing setting of its own: only automatic advance is placed against the end of a file, and that only ever moves forward.
* **Playback speed and frame stepping** for the file that is playing, with the same hotkeys as the Warp Media source. The speed resets to the configured Speed value whenever the playlist moves to another file.
* **Clips of a file**: each file in the list can be played as a clip of itself — from an in point to an out point, or as its last so many seconds — and at a speed of its own, set with `SetClip` over obs-websocket or by a [flow](#warp-flows) with a clip length. A file with nothing set is played whole, at the playlist's speed.
* **Play next**: a file can be put in front of whatever the playlist would play next, with `PlayNext`. Once it has played, the playlist picks up where it would have been.
* **Previous is as quick as Next**: the file before the one that is playing is kept parked on its first frame, so going back to it does not wait for it to open. A file that cannot be opened is skipped as soon as that is known, and a playlist none of whose files can be opened stops rather than going round them.
* **Zoom and framing** for the video that is playing — see [zoom](#zoom) — with the same rule the speed follows: the framing belongs to the video, not to the playlist. Zoom into a corner of one replay and the playlist moves on to the next file at the whole picture, ready to go, however far in the last one was left. The file being transitioned away from keeps the framing it was being watched at, so it plays out the transition as it was on screen while the incoming one is already framed at the top.
* **Audio in the OBS mixer.** The playlist hands OBS the sound of the file that is playing as its own, the way any other source does, so it sits in the audio mixer with a fader, a mute, monitoring, audio filters and its own track routing. The files themselves are played only through the playlist, so the fader is the last word on how loud it is. A transition crossfades the two files it is holding, and a stinger brings its own sound with it.
* **Volume**, from silent to 100% of the audio in the files themselves, and **Transition Volume**, the same for the sound a stinger brings with it, which appears alongside it while a stinger is picked. They are set before the playlist reaches the mixer, so the fader, mute, monitoring and audio filters all still apply on top of them. Both apply live, to the file that is playing as well as to the ones after it, so a playlist of clips recorded far louder than the rest of the show can be turned down without touching the files — and because the two levels are separate, a playlist can be run silent while its stinger still sounds.
* **Hotkeys**: Next Video, Previous Video, Back to First Video, Restart Current Video, Clear Playlist, plus Play/Pause, Stop, the speed hotkeys, and the frame stepping hotkeys.

Clear Playlist empties the source's file list for good — it is written to the scene collection like any other settings change. The cleared file paths are written to the OBS log first, so a playlist cleared by a mis-hit during a show can be rebuilt from there, and the OBS status bar says it was cleared, so a mis-hit does not go unnoticed. It is the one playlist hotkey that is not limited to a source that is on screen: the others drive playback, so they are aimed at what the operator has in front of them, while this one edits the file list, which is most often emptied while the playlist is off air.

Media controls (the ones in the OBS media controls dock) apply to the playlist: restart starts the playlist over from its first file, next and previous move through the list, and the time and duration shown are those of the file that is playing.

Every playlist action can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

### Warp Progress Bar

A source that draws how far through its replay a Warp Media or Warp Playlist source is, to sit over the replay on air. Add it like any other source and pick the source it **follows** from the list at the top of its properties. It is kept by the source itself rather than its name, so renaming the replay source does not lose it, and a source that is not there yet — a scene collection still loading — is picked up as soon as it is.

* **What it measures**: the file that is playing, from its start to its end, or the clip of it when the file is played as one — the same position and length the media controls show. For a playlist it starts again with every file.
* **Style**: a starting point for the look, picked at the top of the properties and changed from there as much as you like. Picking one fills in the colors, effects and proportions; it never touches what the bar measures, where it sits or how long it is.
  * **Broadcast** (the default): a thin bar over a pale glass track, its warm fill lit along the top and glowing at its leading edge, a knob at the position, and the time in a dark chip riding along above it.
  * **Segmented**: a white strip counted off into 24 pieces.
  * **Sport**: slanted ends and a cyan-to-blue fill with a line for a head, the time in a chip slanted to match.
  * **Scoreboard**: a chunky square block in amber on black, with the time printed inside it.
  * **Glass**: a frosted white capsule with a pale rim, the time in dark type on a white chip — at its best over dark pictures.
  * **Neon**: a thin tube of magenta into cyan, glowing hard, with the time in cyan.
  * **Gold**: gold leaf, brightly lit and edged darker, with a pale gold knob.
  * **Minimal**: a red hairline with a dot at the position.
  * **Flat**: plain shapes with no light, glow or shadow.
* **Shape**: a **horizontal bar**, a **vertical bar** or a **ring**. The bars fill left to right or bottom to top, or the other way round; the ring fills clockwise from the top. Bars take a **corner radius** and **slanted ends**, and the ring **rounded ends**. Any of them can be cut into **segments**.
* **Colors and light**: the fill, a **gradient** along it to a second color, the background track (with its own opacity, down to none), a **border** of any width, a **sheen** along the top of the fill, a **glow** off its leading edge, and a soft **drop shadow** that falls only outside the bar, so a see-through track stays clear.
* **Playhead**: none, a line, or a round knob at the position.
* **Color by state**: a fill color of its own while **paused** (stepping pauses), in **slow motion**, played **fast**, and once it has **ended**. The fill fades from one to the next.
* **Time**: the elapsed time, the time remaining, either of them over the total, or a percentage, riding along above the position, above or below the bar, or on it (in the middle of a ring), in any font, and on a **chip** of its own color when wanted — rounded on a rounded bar, square on a square one, and slanted with a slanted one. **Count time at the playback speed** makes it time on the clock rather than in the clip: at 50%, a clip with 4 seconds left reads 8. The bar itself always shows how much of the clip has played.
* **Smooth motion**: the fill glides every frame between the positions the source reports, rather than ticking with the frames of the video. A seek, a frame step or a new file is followed at once.
* **Playlist transitions**: when a playlist moves to another file, the fill eases back from where the outgoing file left it to the incoming one over the length of the transition, so it runs back as the picture changes rather than snapping to the start while the old file is still on screen. A cut, or a switch with no transition, snaps.
* **When there is nothing to show**: what a stopped source, an ended one, and one with nothing loaded each look like — empty, full, the last position, or hidden. By default an ended replay shows full and the bar is hidden otherwise.
* **Hide while off air**: fades the bar out whenever the source it follows is not in the program output, so it can sit in a scene of its own on top of everything and only show while a replay is on air.

The source is as big as the bar or ring is set to be, plus room round it for its shadow, glow and knob, and a band for the time when it is above or below, so it lines up in a scene like any other source and nothing it draws is cut off.

### Warp Status Icon

A source that shows what a Warp Media or Warp Playlist source is doing, as an icon over the replay on air, following a source the same way the [progress bar](#warp-progress-bar) does.

* **The state, all along**: play, pause, stopped and ended. With **Hide while playing normally** on (the default) nothing shows while the replay plays at 100%, so the icon only comes up when something is out of the ordinary; going back into play still flashes the play icon.
* **What just happened, for a moment**: a **frame step** forward or back, with the frames stepped beside it (steps the same way in a row add up, so holding the key reads +7 rather than +1 seven times); a **speed change**, with the new speed; and **next**, **previous** and **restart**. Each is shown for a set time — a second by default — and then gives way back to the state. A playlist rolling on to its next file by itself is not flashed.
* **Speed**: slow motion and fast have icons of their own, shown for as long as the speed is not 100%. With **Show the speed as a badge** on, the badge sits in the corner of play or pause — paused in slow motion reads as both; with it off, the speed icon takes play's place.
* **Style**: a starting point for the look, filled in the same way as the progress bar's.
  * **Chip** (the default): the icon and what it means in one dark pill with an accent rim — `PAUSED`, `SLOW MO 50%`, `FRAME +5` — that widens and narrows smoothly as the words change, with the replay's progress as a fine accent line under them.
  * **Badge**: the icon on a disc whose rim fills round with the replay's progress, the speed as an accent pip in its corner, and the value beside it.
  * **Tag**: a square sports tag — the icon dark on an amber block, the words white on black after it.
  * **Sport**: the tag leaning forward, in cyan and navy.
  * **Glass**: a frosted white pill with dark type.
  * **Neon**: the icon alone in pale cyan, its shadow turned into a glow.
  * **Duotone**: amber icons in two shades of amber, on a dark rounded tile.
  * **Aurora**: icons shaded cyan into violet, with a violet glow and no plate.
  * **Outline**: line art — outlined icons in an empty ring that fills round with the progress.
  * **Minimal**: the icon alone, lifted off the picture by a soft shadow.
  * **Flat**: the icon on a plain disc, with no rounding or motion.

  Several are made to go together: the Sport bar and icon, Scoreboard and Tag, Glass and Glass, Neon and Neon.
* **Text**: what the icon means (`PAUSED`, `SLOW MO`), its value (the speed, or the frames stepped), or both, in any font and color — inside the chip, or to the right of the icon or below it.
* **Icon set**: how the icons themselves are drawn, with any style.
  * **Solid**.
  * **Duotone**: each icon in two tones, split by meaning — the part that says what happened leads in the icon color and the rest supports it in a shade of it, 50% darker unless the **Supporting Shade** says otherwise (below zero, lighter). **Pick the supporting color** gives the supporting part a color of its own instead, for icons in two different colors. The frame bar of a step, the front triangle of fast forward and next, the arrowhead of restart, the triangle of slow motion, the square of ended. Pause splits its two bars; play sits on a disc and stop in a soft square.
  * **Outline**: line art, drawn inside the solid icon's edge so it keeps the same size.
  * **Gradient**: shaded from one color at the top to another at the bottom.

  Badges stay solid, so they read at the size they are.
* **Chip**: a pill, a rounded rectangle, a square or a slanted parallelogram, with the icon on a **block of the accent** at its start when wanted, the way a sports tag is cut.
* **Look**: built-in icons drawn at whatever size the source is, so they stay sharp, with their corners **rounded** off; their color; a **background** circle or rounded square behind them with a color and opacity of its own; an **accent** rim, and the replay's **progress** shown in it; and a **drop shadow**. Changes **fade** from one icon to the next, and a new icon **pops in**.
* **Own images**: any icon can be replaced by an image file of your own, one per state, drawn inside the icon's background.
* **Hide while off air**, the same as the progress bar's.

Each kind of thing it shows — steps, speed, and next/previous/restart — can be turned off on its own.

Both sources follow their source through the `warp_playlist_switched` signal and the [signals](#warp-detection-filter) the Warp sources already emit, so they react to a hotkey, the media controls, a flow, a Warp Detection filter or a [websocket request](#obs-websocket-control) alike. Which source they follow can be changed over obs-websocket too, with [`SetOverlayTarget`](#overlay-requests).

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

* The dock's picture shows **the shot you are building** rather than what is on air, so you judge the framing by looking at it, with an amber border round it and a **STAGED** label beside the **ON AIR** one. On confirm the dock takes its picture from under the zoom rather than through it, so a shot can be lined up **anywhere in the picture**, wherever the one on air happens to be: zoomed into the bottom right, the top left can still be found and framed, and is drawn at the detail it will have on air. Nothing on air moves while you do.
* The corner map shows **both windows**: what is on air in white, and the shot waiting in dashed amber. A preset that is the shot waiting is outlined the same way.
* **Take** puts it up, easing into it the way a preset recall does, and says the zoom it would go to. **Cancel** drops it. Both have hotkeys of their own, so a shot lined up in the dock can be taken from a Stream Deck, and both are websocket requests.

A shot that is never taken is dropped when the video changes — a playlist reaching its next file, or a clip landing in a Warp Media source — since it was composed against a video that is no longer playing. Turning confirm off drops it too: a framing goes to air because somebody took it, never because a setting was switched.

Sources emit `warp_zoom_staged` as a shot is lined up, taken or dropped, so a control surface can follow it. A shot going to air is a `warp_zoom_changed` like any other, which is what a [Warp Detection filter](#warp-detection-filter) reacts to — it fires when the picture actually moves, not when the shot was lined up.

#### The Warp Zoom dock

A dock called **Warp Zoom** is added to OBS when the plugin loads — turn it on under Docks if it is not showing. It is the panel a framing is found from, laid out like a PTZ desk, from the top:

* The **source** being framed, with **Follow** beside it. With Follow down the dock frames whichever Warp source, or source carrying a Warp Zoom filter, is on program right now; leave it up to pick a source from the list and set a framing on a feed before it goes on air. The button at the end of the row puts the picture away and brings it back.
* The **picture** the chosen source is putting out, framed as it is right now, labelled **ON AIR** with the zoom it is at. **Drag it to pan** — the picture follows the cursor, so it is moved rather than aimed — and **roll the wheel to zoom** in and out around it. A dragged pan tracks the hand exactly rather than gliding behind it; everything else in the dock eases. A small map in the corner says where the framing sits in the whole picture once there is anything left out.
* **Direct** or **Confirm**, under the picture: whether a move goes to air as it is made or is [lined up first](#lining-a-shot-up). It is the same per-source setting the source properties carry, read back from the source, and it is on the dock because it is the first thing to know before touching anything. On Confirm, **Cancel** and **Take** stand in the same row the whole time and are greyed until a shot is waiting, so nothing in the dock moves when one is.
* **Frame**: an eight-way pad — the four straight directions and the four corners — around a dot that resets the framing, with the zoom beside it: zoom out and in either side of a readout of how far in the picture is, a **zoom slider**, and **1×, 2×, 4× and 8×** as buttons of their own. The slider runs along the doublings rather than the percentage, so the same length of it is the same step on screen at 600% as at 150%. The pad's buttons are square and grow with the room there is up to a size worth hitting without looking, then stop: the picture is dragged as well, so the pad does not need the dock.
* **Presets**, a tile each: its name, its zoom, the number of the recall hotkey that fires it, and a small map of where in the picture it sits. **Click a tile to recall it** — one click, the picture closing in on the shot (or opening out from it) with every edge travelling one way only, so nothing swings out to the side and back on the way. The preset the picture is on is filled in; one that is waiting to be taken is outlined in dashed amber. **Save framing** stores the framing on screen right now under a name you type, the way a PTZ desk stores a shot. **Edit** turns the tiles from buttons that recall into ones that change a preset — update it to the framing on screen, keeping its name and its hotkey, rename it, or remove it — so nothing that changes a preset can be hit while recalling one; none of the three is offered for the Default preset. Right-clicking a tile offers the same, and Move Up and Move Down, without going into Edit.
* The **speed** the source is playing at: the preset speeds the hotkeys use, the one in use held down, and a slider for anything between — framing and speed being the two things ridden live.

Right-click the dock for **Full** or **Minimal** and for **Confirm framing**, which are the picture button and the Direct / Confirm pair said again for a hand already on the mouse. Minimal drops the picture and keeps everything else for an operator who frames from presets and the pad and would rather have the room. Nothing is rendered while it is minimal, so the dock costs only what its widgets cost. The choice is saved with your OBS user settings rather than with the scene collection, since it is how you like to work rather than anything about the show.

The dock takes the shape it is given. Down the side of the window it is one column. **Along the bottom** — wide and short — its parts stand side by side instead: the picture, then the framing and speed controls, then the presets, each with the dock's whole height.

It shortens as well as narrows. The picture is given the height its own shape comes to at the width it has and no more, so what is left over goes to the presets, and it hands that height back as the dock is dragged shorter. Below the point where it would be a strip too thin to frame a shot in it is put away altogether — the pad, the presets and the speed keep working in a dock too short for a picture, rather than the picture being laid out underneath them. Nothing is rendered while it is away, the same as in Minimal, and it comes back the moment there is room for it again. The presets scroll rather than holding the dock open.

The dock narrows to a strip. The rows of buttons take the next line down rather than holding the panel open, the preset tiles and the preset speeds go from several to a row down to one, the pad and the zoom stop sharing a row and stack, and the source list and the longer labels say as much as there is room for, with the whole of it in the tooltip.

Presets can also be set up by number, which is what the **Zoom Presets…** button in the [Warp window](#warp-flows) opens: every source that can be zoomed, its presets, and the zoom, centre and glide of each one written out.

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

A Warp Playlist also emits one of its own as each file goes up in place of the one before it:

```
warp_playlist_switched(ptr source, string reason, int transition_ms)
```

`reason` is `next` or `previous` when one was asked for (a hotkey, the media controls, a websocket request), `first` when the playlist was sent back to its first file, `start` when it started up — loaded, put on screen, or played after a stop — and `auto` when it moved on by itself as a file ran out. `transition_ms` is how long the transition it runs through takes, or 0 for a cut. The file going up is the one the media controls report from the moment it fires, while the one going out is still on screen under the transition for that long.

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

Eight more apply to a Warp Playlist source, and answer with an error when they are sent to a Warp Media source:

| Request | What it does |
| --- | --- |
| `Next` | Next video |
| `Previous` | Previous video |
| `First` | Back to the first video |
| `RestartCurrent` | Restarts the video that is playing |
| `ClearPlaylist` | Clears the playlist, exactly as the hotkey does — the file list is emptied for good, and written to the log first |
| `PlayNext` | Plays a file next, named by `file` (its path) or `index` (its place in the file list, from 0), ahead of whatever the playlist would have gone on to. Files asked for this way play in the order they were asked for, then the playlist picks up where it would have been. The file has to be in the list |
| `ClearQueue` | Forgets the files asked to play next. The playlist carries on from the file that is playing |
| `SetClip` | Sets what a file in the list, named by `file`, is played as: from `inMs` to `outMs`, or as its last `tailMs`, in milliseconds, and at `speed` percent. Fields left out are 0, which is the start and end of the file and the playlist's own speed; all of them 0 plays the file whole again. It applies the next time the file is played, and is saved with the playlist |

One applies to a Warp Media source only:

| Request | What it does |
| --- | --- |
| `LoadClip` | Loads the file named by `file`, as `playback` says — `keep`, `play` or `hold`, as the [instant replay](#instant-replays) settings describe — at `speed` percent when it is given. `inMs` and `outMs` play it from and to those points, and `tailMs` plays its last that many milliseconds instead. The cut is frame-accurate and nothing is written to disk |

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

#### Overlay requests

Two requests work on a [Warp Progress Bar](#warp-progress-bar) or [Warp Status Icon](#warp-status-icon) instead, named by `sourceName` or `sourceUuid`, so a control surface can move a replay's bar and icon over to whichever replay source is about to go on air:

| Request | Fields | What it does |
| --- | --- | --- |
| `SetOverlayTarget` | `targetName` or `targetUuid` | Has the overlay follow that Warp Media or Warp Playlist source. Leave both out, or empty, to have it follow nothing |
| `GetOverlayTarget` | | Changes nothing, and answers with what the overlay follows |

Both answer with `sourceName`, `sourceUuid` and `sourceKind` for the overlay, and `targetName` and `targetUuid` for what it follows — empty when it follows nothing, and the name empty as well when the source it follows is not in the scene collection.

#### Responses

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

Every response also reports `zoom`, `zoomX` and `zoomY` — how the source is framed, in percent — along with `zoomFilter` when the framing lives in a filter rather than in the source itself, so a zoom can be followed without asking for it separately. `zoomConfirm` says whether that source lines shots up, and `zoomStaged` whether one is waiting; when one is, `zoomStagedZoom`, `zoomStagedX` and `zoomStagedY` are the shot it would take. A Warp Playlist source also reports `playlistIndex` (-1 when nothing is playing), `playlistLength`, `currentFile`, and `playlistQueued`, the number of files asked to play next. A request that could not be carried out — no such source, a source that is not a Warp source, or a value outside the range the action takes — answers with `"success": false` and an `error` saying what was wrong, and changes nothing.

The requests do exactly what the matching hotkeys do, including emitting the signals above, so a Warp Detection filter reacts to a speed change driven over the websocket the same way it reacts to the hotkey. The difference is that a hotkey only applies to a source that is on screen, because the operator is pressing it at whatever is in front of them, while a request names the source it means and is carried out whether or not it is being shown. There are two exceptions, one either way. `Restart` on a Warp Media source only restarts playback while the source is being shown, over the websocket as from the hotkey. `ClearPlaylist` edits the file list rather than driving playback, so its hotkey is carried out whether or not the playlist is on screen, the same as the request.

Three more work on a [Warp flow](#warp-flows) rather than on a source, and name it with `flowName` (or `flowId`) instead of `sourceName`:

| Request | What it does |
| --- | --- |
| `SaveToFlow` | Saves the replay buffer and keeps the clip for that flow, exactly as its Save Replay hotkey does. Answers with an error when the replay buffer is not running |
| `AddLastReplayToFlow` | Puts the replay that was saved last in that flow, as its Add Last Saved Clip hotkey does |
| `GetFlows` | Changes nothing, and answers with every flow in the scene collection |

`SaveToFlow` and `AddLastReplayToFlow` also take `seconds`, which asks for a clip of that length rather than the one the flow keeps — `0` for the whole of what the replay buffer wrote, up to 3600, and anything a [clip length](#clip-lengths) can be. A length asked for this way is the length of that clip, so the flows it is handed on to are fed the same length. Left out, each flow keeps what it is set to.

```json
{"vendorName": "warp", "requestType": "SaveToFlow",
 "requestData": {"flowName": "Match Replays", "seconds": 10}}
```

They answer with the flow they applied to, how much is in the playlist it feeds, and the lengths it keeps:

```json
{
  "success": true,
  "flowId": "flow_…",
  "flowName": "Match Replays",
  "flowKind": "replay",
  "targetName": "Replay Feed",
  "clipCount": 4,
  "clipLength": 10,
  "clipLengths": [{"seconds": 5}, {"seconds": 20}],
  "seconds": 10
}
```

`clipLength` is the flow's own length in seconds, with `0` for the whole buffer, and `clipLengths` the ones it offers on top of it. `seconds` is only there when the request asked for a length of its own. `GetFlows` answers with `flows`, an array of those same objects without that field, and takes no fields.

The playlist actions that have no counterpart in OBS's media control API are proc handlers on the source, so scripts can call them too, and so can anything else that can reach the source:

```
warp_set_speed(int speed)          warp_playlist_first()
warp_adjust_speed(int delta)       warp_playlist_restart_current()
warp_get_speed(out int speed)      warp_playlist_clear()
warp_step_frames(int frames)       warp_playlist_status(out int index, out int count, out string current_file, out int queued)
warp_playlist_queue(string path, int index, out bool queued)
warp_playlist_clear_queue()
warp_playlist_set_clip(string path, int in_ms, int out_ms, int tail_ms, int speed)
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

The first four playlist procs are on the Warp Media source as well, along with three of its own:

```
warp_media_load(string path, int speed, string playback, int in_ms, int out_ms, int tail_ms, out int seq)
warp_media_ready(out bool ready, out bool failed, out int seq)
warp_media_close()
```

`warp_media_load` loads a file into the source and says what playback does with it — `keep`, `play` or `hold`, as the [Instant Replay flow](#instant-replays) settings describe — and, with `in_ms`, `out_ms` or `tail_ms`, which stretch of the file is the clip. It is what an instant replay flow calls, and what the Warp Playlist loads each of its files with, and it emits the source's `loaded` media action. It answers with `seq`, the number of the load; `warp_media_ready` says whether that load has a picture in the source yet, or `failed` once the file has turned out not to open. `warp_media_close` closes the file and clears the picture. Play, pause, stop, restart, next, previous and seeking are OBS's own media controls on both sources, so obs-websocket's built-in `TriggerMediaInputAction`, `SetMediaInputCursor` and `GetMediaInputStatus` requests work on them too.

### Warp flows

A **Warp** entry in the Tools menu opens the Warp window, laid out for the middle of a show as much as for setting one up:

* **Along the top**, the replay buffer: whether it is running, with a button to start or stop it. No flow can take a clip without it, so while it is stopped a banner across the window says so and offers to start it, and stopping it is asked about first, since it throws away what the buffer is holding. **Add Flow…**, **Zoom Presets…** — which opens the [zoom presets](#the-warp-zoom-dock) of every source that can be framed — and a **?** that says what flows are sit along the top as well.
* **The flow list**: each flow with a switch to turn it on and off on the spot, its name with its kind's icon, what it does in a few words underneath — where its clips come from, its order (or, for an instant replay, what playback does with a clip), its length, and the flows it hands its clips on to — the source it feeds, and how many clips are in it, with a bar showing how full a list with a limit is, turning amber once it is dropping its oldest clip. A source that has gone is said in red. A flow fed by another is set in under it, joined to it by a line, so a replay list and the highlight list it fills read as a pair. Each row has a **Save** button, labelled with the key its Save Replay hotkey is bound to, and a menu with the flow's other lengths, Add Last Saved Clip, Properties and Remove.
* **Beside the list**, everything about the flow that is picked: all of its settings in full, the flows that feed it, and every hotkey it registers with the key bound to it, or *Not set*. Save Replay, Add Last Clip, Properties and Remove are there too, and a flow whose source has gone offers to pick another one.
* **Along the bottom**, the clip that was saved last: how long ago it was handed out, and every flow it reached, with the ones that could not take it in red — the same news the [status bar](#warp-flows) gives, kept where it can be read after the status bar has moved on.

With no flows in the scene collection, the list says what flows are and offers to add the first one. Double-clicking a flow opens its properties.

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
* **Clip length**: how much of the replay buffer this flow keeps — all of it, which is what it does by default, or the last so many seconds of it. See [clip lengths](#clip-lengths) below.
* **Other lengths**: lengths in seconds, separated by commas, that this flow offers on top of the one above. Each gets a **Save Replay** hotkey of its own, so a length can be picked in the moment rather than set in advance.
* **Also feeds**: the flows this one hands every clip it takes on to, so a highlight reel builds itself alongside a replay feed. Links are followed through further links, and a flow already being fed is not fed twice.
* **Take clips**: a flow that is switched off takes nothing itself, but still passes clips on to the flows it is linked to.

Each flow registers two hotkeys of its own, named after it in Settings → Hotkeys, and one more for each of the other lengths it offers:

* **Warp: Save Replay to *flow*** — saves the replay buffer and keeps the clip for that flow, at the length that flow keeps. Pressed while the replay buffer is not running, it says so and offers to start it there and then.
* **Warp: Add Last Saved Clip to *flow*** — puts the replay that was saved last in that flow without saving a new one, for a clip worth keeping that nobody knew about in advance.
* **Warp: Save *10s* Replay to *flow*** — one for each of the flow's other lengths, saving the buffer and keeping that much of it.

The keys they are bound to are saved with the flow, so a flow loads with its hotkeys the way it was left. A length that is still offered after the flow has been edited keeps the key it was bound to.

A clip landing in a playlist disturbs nothing about playback: the file list is edited the way the playlist's own Clear Playlist edits it, so whatever is on screen plays out untouched and the new clip is reached in its turn. A clip that is already in the list is not added a second time, and a flow whose playlist source has gone says so in the OBS log rather than losing the clip quietly.

Flows are saved with the scene collection, alongside the sources they feed, so an event day's collection carries its own feed setup and switching collections switches flows.

Every flow action can also be driven from outside OBS — see [obs-websocket control](#obs-websocket-control).

What a flow is doing is said in the **OBS status bar**, along the bottom of the main window, where OBS itself says a replay was saved: a save on its way for a flow, the flows a clip landed in, and — held up longer, since it is the one that needs doing something about — a flow that could not take a clip because its source has gone. It works the same whether the flow was driven by a hotkey, by obs-websocket or by listening. The OBS log stays the full record; the status bar is the line an operator can glance at mid-show.

#### Clip lengths

The OBS replay buffer writes everything it is holding, so a save is as long as the buffer is — usually long enough for the worst case, which is longer than most moments are worth. A flow set to keep less than that is handed **the file the buffer wrote, with the length it keeps**, and its source plays that much of the end of the file: a Warp Media source loads it as a clip of the file, and a Warp Playlist source lists the file and keeps the length beside it. Nothing is cut, copied or encoded again, so a clip lands the moment it is saved, and **the file OBS wrote is left exactly as it is**.

The clip starts on the exact frame, however the recording was encoded — there is no rounding back to a keyframe. A length longer than the buffer is holding is not an error either: the whole clip is played.

Two ways to ask for one:

* **The flow's own length**, in its properties. Every save that flow takes is kept to it, including the ones it takes by listening, so a replay list keeping 10 seconds and a highlight list keeping 30 are both fed from the same save, each getting its own.
* **A length hotkey**, from the flow's *Other lengths*. A length asked for out loud is the length of that clip, so the flows it is handed on to are fed the same length rather than each taking their own — press *Save 5s Replay to Match Replays* and the highlight list linked to it gets the same five seconds.

A playlist holds each file once, so two flows feeding the same playlist with different lengths share the entry for that file, and the one that lands last sets its length.

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

The Warp window shows an instant replay flow as *Loaded* or *Empty* rather than as a count, and what it does with a clip where a list shows its order.

## Roadmap

Ideas that are designed but not built yet live in [ROADMAP.md](ROADMAP.md), with notes to pick them up from:

* **In-memory replay buffer** — keep the last few minutes of the stream's encoded packets in memory and play replays straight out of them, with no save to wait for.
* Background probing of playlist files, a playlist panel in the Warp window, and the render tests in CI.

## Building

This plugin is based on the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) and builds the same way. See the template's [wiki](https://github.com/obsproject/obs-plugintemplate/wiki) for build system requirements and options.

In addition to the template's requirements, the plugin links against FFmpeg (`avcodec`, `avdevice`, `avfilter`, `avformat`, `avutil`, `swscale`). On Linux, install the corresponding `-dev` packages; on Windows and macOS the obs-deps prebuilt dependencies already include FFmpeg.

Quick start on Linux:

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

GitHub Actions workflows build the plugin for Windows, macOS, and Ubuntu on every push and pull request.

### Render tests

`tests/` holds a harness that runs libobs headless on Linux, loads the plugin, plays real clips through Warp Media and Warp Playlist sources, and checks what the Warp Progress Bar and Warp Status Icon draw, pixel by pixel. It is run by hand, not in CI:

```sh
tests/run-render-tests.sh
```

See [tests/README.md](tests/README.md) for what it needs installed and what it checks.

## License

GNU General Public License v2.0 (or later). `src/obs-websocket-api.h` is the header [obs-websocket](https://github.com/obsproject/obs-websocket) ships for plugins that add vendor requests, taken as-is (GPL v2.0 or later). The `src/media-playback` directory is adapted from [obs-studio](https://github.com/obsproject/obs-studio)'s `deps/media-playback` library (see `src/media-playback/LICENSE`), rebuilt around a single playback clock and a command queue, with on-the-fly speed changes, frame-accurate seeking and stepping, clips of a file, and files loaded into a long-lived engine. `src/warp-source.c` is adapted from obs-studio's Media Source (`plugins/obs-ffmpeg/obs-ffmpeg-source.c`).
