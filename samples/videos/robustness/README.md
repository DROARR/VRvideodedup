# Robustness fixtures

Small files reproducing scan failures seen on large VR libraries. Used by `test_repo_vr_robustness`.

- `nice-no-seek-index.mkv`: `Nice_383p_500kbps.mp4` remuxed as a live Matroska stream
  (`ffmpeg -i Nice_383p_500kbps.mp4 -c copy -f matroska -live 1`). It has no duration and no seek index, so the
  duration is estimated from the file size, overshoots, and the end capture used to fail.
- `vr-8k-tiny.mp4`: 3 s, 7680x3840 HEVC with almost no detail (tiny file, real 8K decode). Captures must be
  downscaled so that collages of 8K frames do not use gigabytes of memory.

The corrupt-packet case is generated at test time by overwriting bytes in a copy of `Nice_720p_1000kbps.mp4`.
