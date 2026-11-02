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

## GPU direct decoding: verification, and macOS

**Where it stands:** built, experimental and off by default (*Draw decoded video straight from the GPU*, under hardware decoding, on both sources).

* **Windows (D3D11):** the decoder runs on its own D3D11 device, made on the adapter OBS draws with. Each frame is copied on the GPU from the decoder's texture array into an NV12 or P010 texture OBS shares with it under a keyed mutex — the same kind of texture OBS hands its own frames to hardware encoders in — and converted with the plugin's copy of OBS's shaders. `src/warp-video-gpu.c`.
* **Linux (VAAPI):** each frame's surface is exported as dma-bufs and its planes imported into OBS's EGL context with `gs_texture_create_from_dmabuf`.
* **Fallback:** a frame that cannot be imported is copied back on the graphics thread and drawn from the copy, the source logs that it fell back, and the engine stops handing frames out on the GPU. Anything that goes wrong costs speed, not picture.

**Still to do:**

* **Verify on real hardware.** The paths compile on all three CI platforms and the fallback is tested under AddressSanitizer (with Vulkan frames from lavapipe, which the import refuses), but no import has run on a real GPU yet. Worth trying: Windows with NVIDIA, AMD and Intel, including a laptop where OBS draws on a different adapter from the default; Linux with Intel and AMD (Mesa VAAPI); 8-bit H.264 and HEVC, and 10-bit HEVC (P010). The OBS log says *drawing hardware-decoded video straight off the GPU* when it works and *copying it instead* when it does not.
* **Measure it.** CPU use and GPU copy-engine load with it on and off, for a 4K60 clip.
* **Once it is proven,** consider turning it on by default.
* **macOS.** libobs only makes textures from single-plane IOSurfaces (`gs_texture_create_from_iosurface`), and VideoToolbox hands out two-plane NV12. Two ways forward: ask VideoToolbox for BGRA output (a `hw_frames_ctx` with `sw_format = AV_PIX_FMT_BGRA` from `get_format`) and draw it as a rectangle texture — at the cost of VideoToolbox doing the colour conversion rather than OBS's shaders, and of HDR; or a Metal/IOSurface import of each plane, which libobs would need to grow.
* **NVIDIA on Linux** decodes with CUDA, not VAAPI; a CUDA→EGL interop path (`cuGraphicsEGLRegisterImage`) would be its own piece of work.

## Smaller follow-ups

* **Background probing of playlist files** — duration, resolution, frame rate and a thumbnail read on a worker thread as files are added, for the total run time, flagging unplayable files early, and a playlist panel in the Warp window.
* **A playlist panel in the Warp window** — the list, the play-next queue, and each file's in/out points and speed, edited directly rather than over obs-websocket.
* **Deinterlacing** — the Warp Media source draws itself, so OBS's own right-click Deinterlacing no longer applies. Replay buffer recordings are progressive, but interlaced files dropped into a playlist would need a deinterlacing pass of Warp's own (FFmpeg's `yadif`/`bwdif` in the engine, or a shader).
* **Unit tests in CI** — the engine and the sources were tested during development with a harness that runs libobs headless under Xvfb and reads the program output back. Checking it in, and running it on the Ubuntu CI job, would keep those checks.
