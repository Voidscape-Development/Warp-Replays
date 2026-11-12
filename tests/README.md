# Render tests

A harness that runs libobs headless, loads the plugin the way OBS does, plays real clips through Warp Media and Warp Playlist sources, and checks what the **Warp Progress Bar** and the **Warp Status Icon** draw, pixel by pixel. It also covers what they depend on in the sources: the `warp_playlist_switched` signal and its reasons, and the Warp Media source not reporting a frame step's own pause as an operator's.

It is run by hand. It is not part of CI yet; see [ROADMAP.md](../ROADMAP.md).

## Running it

On Linux, from the top of the repository:

```sh
tests/run-render-tests.sh            # add --verbose for all of libobs' log
```

The script:

1. builds the plugin into `build_tests/plugin` with `ENABLE_FRONTEND_API=OFF` and `ENABLE_QT=OFF`, since there is no OBS window to load the UI into;
2. builds the harness (`tests/warp-render-tests.c`) against the libobs installed on the system;
3. makes two four-second test clips and a small image with FFmpeg, in `build_tests/media`;
4. runs the harness under `xvfb-run`, which gives libobs an X display to make its OpenGL context on.

It prints each check as it goes, ends with a count, and exits non-zero if any check failed. Every picture it checked is left in `build_tests/render-out` as a PNG, to look at.

## What it needs

* The plugin's own build requirements (see the main [README](../README.md#building)).
* OBS Studio installed from the [OBS PPA](https://launchpad.net/~obsproject/+archive/ubuntu/obs-studio) (`obs-studio`), which brings libobs and its CMake package along with the `obs-transitions` and `text-freetype2` plugins the tests load: the playlist's transitions, and the text the overlays draw their numbers with. Somewhere else, point the script at them with `OBS_PLUGINS_DIR` and `OBS_DATA_DIR`.
* `xvfb` (for `xvfb-run`) and Mesa's OpenGL, which a desktop install already has.
* `ffmpeg` on the command line, with `libx264`.
* At least one font fontconfig can find.

On Ubuntu 24.04 that is:

```sh
sudo add-apt-repository ppa:obsproject/obs-studio
sudo apt install obs-studio xvfb ffmpeg ninja-build build-essential libsimde-dev \
  libavcodec-dev libavdevice-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

## What it checks

* **Progress bar**: the fill follows the position of a Warp Media source; reversed and vertical bars fill from the other end; the gradient runs along the fill; the paused color is taken while paused; a frame step lands the fill on the stepped frame at once; the playhead, the border and the time are drawn where they belong; an ended source shows full, or hides when set to; the bar hides while its source is off program; a ring fills clockwise from the top with its middle left open for the time.
* **Status icon**: nothing while playing at 100%; pause; a step flashing its own glyph with the frames beside it, and giving way back to pause; slow motion with its badge, or on its own; an image of the operator's own in place of a glyph; the value below the icon; stopped.
* **Playlist**: its first file reported as `start`, Next as `next` with the transition's length, moving on by itself as `auto`, Previous as `previous`; the bar easing back over the transition and settling on the new file; the icon flashing the skip.
* **Styles**: every bar style draws, as a bar and as a ring, picked through the properties the way the Style list picks it; the broadcast bar is given room for its shadow and glow; the chip opens out to take its text and closes up to fit shorter text; the badge's rim fills with the progress; the scoreboard prints its time inside the bar; the tag and sport icons sit their glyph on a block of the accent.
* **The target** is kept by UUID through a rename, and listed under its new name.

The pixel checks are written against the Flat look, which every test that is not about styles starts from.

The PNGs are read back from libobs premultiplied by their alpha, so a see-through track or plate looks darker in an image viewer than it does on air; divide the color by the alpha before laying one over a picture to judge it.

The `SetOverlayTarget` and `GetOverlayTarget` websocket requests are not covered: obs-websocket needs the OBS window to load.
