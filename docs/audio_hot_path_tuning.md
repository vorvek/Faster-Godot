# Audio Hot-Path Tuning

## Intent

Reduce per-frame audio thread overhead in active playback paths without changing
the public audio API.

## Changes

- `modules/mp3/audio_stream_mp3.{h,cpp}`
  - Decodes MP3 playback in bounded blocks into a preallocated per-playback
    scratch buffer instead of calling `drmp3_read_pcm_frames_f32()` once per
    output frame.
  - Keeps normal loop and beat-loop fade behavior, including crossfade data
    capture at beat-loop boundaries, while clamping invalid loop targets so the
    audio thread cannot spin without producing samples.

## Pros

- Cuts decoder function-call overhead on the audio mix thread.
- Reuses scratch storage across mix callbacks instead of growing it from the
  audio thread.

## Validation

- Windows editor dev build.
- Static review of loop, beat-loop fade, EOF, channel, and playback API
  behavior.
