#!/usr/bin/env bash

# Regenerates the fixtures for matching clips cut from a longer video by picture alone.
#
# Every clip gets a different soundtrack from its source, which is the case sound matching cannot handle: replaced
# music, a voice-over, or sound edited separately. The picture is a slow, steady zoom, like the mostly static camera
# of VR footage, so neighbouring keyframes of one video look alike while distant moments do not. Copies are encoded
# with other keyframe spacings than the source, as re-encodes are, so their keyframes fall on other moments.

set -euo pipefail

fixture_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ffmpeg_bin="${FFMPEG_BIN:-ffmpeg}"

if ! command -v "$ffmpeg_bin" >/dev/null 2>&1; then
    echo "FFmpeg is required to regenerate the picture cut fixtures." >&2
    exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -r "$temporary_dir"' EXIT

run_ffmpeg() {
    "$ffmpeg_bin" -hide_banner -loglevel error -y "$@"
}

# A different short tone pattern per seed: stands in for whatever new sound a cut copy was given.
tone() {
    local seed="$1" seconds="$2"
    echo "aevalsrc='0.2*sin(2*PI*t*(300+900*(abs(sin(floor(t*3)*17.17+${seed})*7777.7)-floor(abs(sin(floor(t*3)*17.17+${seed})*7777.7)))))':s=22050:d=${seconds}"
}

encode() {
    local keyframe_spacing="$1"
    shift
    echo -c:v libx264 -preset veryslow -crf 40 -pix_fmt yuv420p -g "$keyframe_spacing" \
        -c:a aac -b:a 12k -ac 1 -ar 22050 -movflags +faststart "$@"
}

# The long source: 60 s of a slow zoom into one region of the Mandelbrot set, keyframes every 5 s.
source="$temporary_dir/source.mkv"
run_ffmpeg -f lavfi -t 60 -i "mandelbrot=size=128x72:rate=8:start_scale=3:end_scale=0.3:end_pts=800" -c:v ffv1 "$source"

run_ffmpeg -i "$source" -f lavfi -i "$(tone 11 60)" -shortest $(encode 40) "$fixture_dir/p-full.mp4"

# Clips cut from it, each with new sound.
# Re-encoded smaller, with keyframes every 3 s instead of 5 s.
run_ffmpeg -ss 12 -to 42 -i "$source" -f lavfi -i "$(tone 12 30)" -vf "scale=96:54" -shortest $(encode 24) \
    "$fixture_dir/p-cut-reencoded.mp4"
# Cut without re-encoding the picture, as lossless cutters do: it starts on a keyframe of the source.
run_ffmpeg -ss 20 -to 45 -i "$fixture_dir/p-full.mp4" -f lavfi -i "$(tone 13 25)" -map 0:v -map 1:a -c:v copy \
    -c:a aac -b:a 12k -ac 1 -shortest -movflags +faststart "$fixture_dir/p-cut-lossless.mp4"
# Cut and remastered: regraded colours, upscaled, keyframes every 4 s.
run_ffmpeg -ss 30 -to 55 -i "$source" -f lavfi -i "$(tone 14 25)" \
    -vf "eq=brightness=0.05:saturation=1.3:gamma=1.1,scale=192:108" -shortest $(encode 32) \
    "$fixture_dir/p-cut-remaster.mp4"
# Two parts of the source joined, the middle left out.
run_ffmpeg -ss 5 -to 20 -i "$source" -c copy "$temporary_dir/part1.mkv"
run_ffmpeg -ss 35 -to 52 -i "$source" -c copy "$temporary_dir/part2.mkv"
run_ffmpeg -i "$temporary_dir/part1.mkv" -i "$temporary_dir/part2.mkv" \
    -filter_complex "[0:v][1:v]concat=n=2:v=1:a=0[v]" -map "[v]" -c:v ffv1 "$temporary_dir/parts.mkv"
run_ffmpeg -i "$temporary_dir/parts.mkv" -f lavfi -i "$(tone 15 32)" -shortest $(encode 28) \
    "$fixture_dir/p-two-parts.mp4"

# Negatives.
# The same kind of picture from another region: alike in style, different content.
run_ffmpeg -f lavfi -t 40 \
    -i "mandelbrot=size=128x72:rate=8:start_x=-0.75:start_y=0.1:start_scale=3:end_scale=0.3:end_pts=800" \
    -f lavfi -i "$(tone 16 40)" -shortest $(encode 40) "$fixture_dir/n-other-region.mp4"
# Two scenes in the same "room": the source's first frame held still, each with a small object moving along its own
# path. Like a fixed VR camera filming two scenes in one room, every moment of one looks alike every moment of the
# other (about 56/64 bits), so keyframes alone line them up at any offset: only the frame by frame check, which
# needs one offset to fit several moments at once, can tell them apart.
run_ffmpeg -i "$source" -frames:v 1 "$temporary_dir/still.png"
run_ffmpeg -loop 1 -t 40 -framerate 8 -i "$temporary_dir/still.png" -f lavfi -i "$(tone 17 40)" \
    -vf "drawbox=x='mod(t*9,118)':y=10:w=8:h=8:color=white@0.9:t=fill" -shortest $(encode 40) \
    "$fixture_dir/n-same-set-other-action.mp4"
run_ffmpeg -loop 1 -t 40 -framerate 8 -i "$temporary_dir/still.png" -f lavfi -i "$(tone 18 40)" \
    -vf "drawbox=x='118-mod(t*5,118)':y=50:w=8:h=8:color=white@0.9:t=fill" -shortest $(encode 40) \
    "$fixture_dir/n-same-set-another-action.mp4"
