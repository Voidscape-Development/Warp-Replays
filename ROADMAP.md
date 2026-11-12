# Warp roadmap

Work that has been thought through but not built yet, with enough of the design written down to pick it up from. Newest thinking first within each item; nothing here is promised for any release.

## In-memory replay buffer

**What:** keep the last few minutes of the stream's encoded video and audio in memory inside Warp, and play replays straight out of that, instead of waiting for OBS's replay buffer to write a file and then opening it.

**Why:**

* A replay is ready the instant the operator asks for it. Today a save has to finish writing (the whole buffer, every time) before a flow can load it.
* A replay can be played while the moment is still happening — the buffer keeps growing underneath it.
* "Replay the last 8 seconds" no longer depends on the length OBS's replay buffer was set to: any length up to what is held in memory.
* Saving to disk becomes optional and can happen in the background, from the same packets, only for the clips worth keeping.

**How it would fit:**

* **Capture.** A Warp-owned `obs_output` registered with `OBS_OUTPUT_ENCODED | OBS_OUTPUT_AV`, attached to the same video and audio encoders the stream or recording uses (an encoder can feed several outputs), so nothing is encoded twice. Its `encoded_packet` callback appends packets to a ring.
* **The ring.** Packets kept per track, with their timestamps, flags and the encoder's extradata (SPS/PPS/VPS for H.264/HEVC, the AV1 sequence header). Trimmed from the front a whole GOP at a time, so the oldest packet held is always a keyframe. Sized by duration with a byte cap (bitrate × seconds; at 20 Mbps, 3 minutes is about 450 MB).
* **Playback.** The engine already separates reading from decoding: `prepare_frames` pulls packets and `mp_decode_push_packet` hands them to the decoders. A load would take a "ring" input instead of a path — a reader that walks the ring from the keyframe at or before the in point, rather than `av_read_frame` on a file. In/out points, tails, seeking, stepping, speed and pitch work unchanged, since they are all done on decoded frames. The decoder is reused across clips, as it already is, because the encoder settings do not change.
* **Growing clips.** A clip whose out point is "now" keeps reading as packets arrive; the reader waits for the next packet rather than treating the end of the ring as the end of the clip.
* **Saving.** Remuxing a range of the ring to a file (`avformat` with the packets as they are) on a worker thread, which is what a flow would do for the clips it keeps. Flows would learn to feed a source from the ring and to save only when asked.

**Things to settle first:**

* Which encoders to tap when the stream and the recording use different ones, and what to do when neither is running (start a dedicated encoder, or say so).
* Memory: a sensible default length and cap, and what the operator sees when it is full.
* B-frames: packets arrive in decode order with DTS ≠ PTS; the reader has to start at a keyframe and the in point has to be measured in PTS.
* Multiple audio tracks: which one a replay plays.
* Interaction with OBS's own replay buffer (both can run; flows could prefer the ring when it is available).

## Smaller follow-ups

* **Background probing of playlist files** — duration, resolution, frame rate and a thumbnail read on a worker thread as files are added, for the total run time, flagging unplayable files early, and a playlist panel in the Warp window.
* **A playlist panel in the Warp window** — the list, the play-next queue, and each file's in/out points and speed, edited directly rather than over obs-websocket.
* **Render tests in CI** — `tests/` now holds a harness that runs libobs headless under Xvfb, drives real Warp sources and reads the overlays back (see `tests/README.md`), run by hand. Running it on the Ubuntu CI job needs Xvfb, the OBS PPA's `obs-studio` (for libobs, `obs-transitions` and `text-freetype2`) and FFmpeg's command line on the runner, and extending it to the engine and the sources themselves would cover what was checked by hand during development.
