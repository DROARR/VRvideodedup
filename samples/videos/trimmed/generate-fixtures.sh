#!/usr/bin/env bash

# Regenerates the fixtures for matching trimmed, cut and remastered copies by their soundtrack.
#
# Every output is re-encoded from a lossless intermediate, as real copies are: a copy that merely
# stream-copies the original would share its exact bytes and prove nothing about robustness.
# The soundtracks are synthetic "melodies" of pseudo-random notes, so each seed produces sound
# that never repeats within or across fixtures, like real recordings, while staying reproducible.

set -euo pipefail

fixture_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ffmpeg_bin="${FFMPEG_BIN:-ffmpeg}"

if ! command -v "$ffmpeg_bin" >/dev/null 2>&1; then
    echo "FFmpeg is required to regenerate the trimmed copy fixtures." >&2
    exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -r "$temporary_dir"' EXIT

run_ffmpeg() {
    "$ffmpeg_bin" -hide_banner -loglevel error -y "$@"
}

# Small, low bitrate outputs keep the tracked total small, and low bitrate audio is also a harsher
# test of the fingerprint than the copies found in real libraries.
encode_output=(
    -c:v libx264 -preset veryslow -crf 45 -pix_fmt yuv420p -g 20
    -c:a aac -b:a 16k -ac 1 -ar 22050
    -movflags +faststart
)

# Three voices of pseudo-random notes; the seed changes every note frequency.
melody() {
    local seed="$1"
    local note1="200+1600*(abs(sin(floor(t*5)*12.9898+${seed})*43758.5453)-floor(abs(sin(floor(t*5)*12.9898+${seed})*43758.5453)))"
    local note2="300+2400*(abs(sin(floor(t*3)*78.233+${seed})*12345.6789)-floor(abs(sin(floor(t*3)*78.233+${seed})*12345.6789)))"
    local note3="80+300*(abs(sin(floor(t*2)*39.425+${seed})*24680.1357)-floor(abs(sin(floor(t*2)*39.425+${seed})*24680.1357)))"
    echo "0.30*sin(2*PI*t*(${note1}))*exp(-4*mod(t*5,1))+0.22*sin(2*PI*t*(${note2}))*exp(-3*mod(t*3,1))+0.18*sin(2*PI*t*(${note3}))"
}

# Lossless source: picture from a lavfi video source, sound from a melody seed.
make_source() {
    local output="$1" video="$2" seed="$3" seconds="$4"
    run_ffmpeg -f lavfi -t "${seconds}" -i "${video}" \
        -f lavfi -i "aevalsrc='$(melody "$seed")':s=22050:d=${seconds}" \
        -c:v ffv1 -c:a pcm_s16le -shortest "$output"
}

concat_sources() {
    local output="$1"
    shift
    local inputs=() filter="" index=0
    for source in "$@"; do
        inputs+=(-i "$source")
        filter+="[${index}:v][${index}:a]"
        index=$((index + 1))
    done
    run_ffmpeg "${inputs[@]}" -filter_complex "${filter}concat=n=${index}:v=1:a=1[v][a]" \
        -map "[v]" -map "[a]" -c:v ffv1 -c:a pcm_s16le "$output"
}

a_source="$temporary_dir/a.mkv"
make_source "$a_source" "testsrc2=size=128x72:rate=8" 1 48
# A studio intro: identical picture and sound at the start of unrelated videos.
jingle_source="$temporary_dir/jingle.mkv"
make_source "$jingle_source" "smptehdbars=size=128x72:rate=8" 9 6

# Positives: every one of these is the same content as a-full.
run_ffmpeg -i "$a_source" "${encode_output[@]}" "$fixture_dir/a-full.mp4"
run_ffmpeg -i "$a_source" -ss 8 "${encode_output[@]}" "$fixture_dir/a-trim-start.mp4"
run_ffmpeg -i "$a_source" -ss 6 -to 41 "${encode_output[@]}" "$fixture_dir/a-trim-both.mp4"
concat_sources "$temporary_dir/a-intro.mkv" "$jingle_source" "$a_source"
run_ffmpeg -i "$temporary_dir/a-intro.mkv" "${encode_output[@]}" "$fixture_dir/a-intro.mp4"
run_ffmpeg -i "$a_source" -to 20 -c copy "$temporary_dir/a-part1.mkv"
run_ffmpeg -i "$a_source" -ss 30 -c copy "$temporary_dir/a-part2.mkv"
concat_sources "$temporary_dir/a-midcut.mkv" "$temporary_dir/a-part1.mkv" "$temporary_dir/a-part2.mkv"
run_ffmpeg -i "$temporary_dir/a-midcut.mkv" "${encode_output[@]}" "$fixture_dir/a-midcut.mp4"
# Remaster: regraded colours, upscaled picture, quieter and filtered sound in another codec.
run_ffmpeg -i "$a_source" \
    -vf "eq=brightness=0.06:saturation=1.4:gamma=1.15,scale=256:144" \
    -af "volume=0.6,highpass=f=150" \
    -c:v libx264 -preset veryslow -crf 45 -pix_fmt yuv420p -g 20 \
    -c:a libmp3lame -b:a 32k -ac 1 -ar 22050 -movflags +faststart \
    "$fixture_dir/a-remaster.mp4"

# A short family-style clip and a trimmed copy of it.
f_source="$temporary_dir/f.mkv"
make_source "$f_source" "mandelbrot=size=128x72:rate=8" 5 16
run_ffmpeg -i "$f_source" "${encode_output[@]}" "$fixture_dir/f-short-full.mp4"
run_ffmpeg -i "$f_source" -ss 3 -to 13 "${encode_output[@]}" "$fixture_dir/f-short-trim.mp4"

# Negatives.
# Same soundtrack as A but a different picture, like two phones filming the same moment.
make_source "$temporary_dir/d.mkv" "life=size=128x72:rate=8:mold=10:ratio=0.1:seed=1:life_color=#00ff00" 1 48
run_ffmpeg -i "$temporary_dir/d.mkv" "${encode_output[@]}" "$fixture_dir/d-same-sound-other-picture.mp4"
# Two unrelated videos that open with the same studio intro (also shared with a-intro).
make_source "$temporary_dir/c1-body.mkv" "life=size=128x72:rate=8:mold=10:ratio=0.1:seed=2:life_color=#ff8000" 3 30
concat_sources "$temporary_dir/c1.mkv" "$jingle_source" "$temporary_dir/c1-body.mkv"
run_ffmpeg -i "$temporary_dir/c1.mkv" "${encode_output[@]}" "$fixture_dir/c1-intro-other.mp4"
make_source "$temporary_dir/c2-body.mkv" "cellauto=size=128x72:rate=8:rule=110:seed=3" 4 30
concat_sources "$temporary_dir/c2.mkv" "$jingle_source" "$temporary_dir/c2-body.mkv"
run_ffmpeg -i "$temporary_dir/c2.mkv" "${encode_output[@]}" "$fixture_dir/c2-intro-other.mp4"
