#!/usr/bin/env python3
"""Builds the movies the firmware plays in its movie mode.

The ESP32-S3 decodes H.264 in software and drives a 240x240 panel, so a movie
has to be built for that panel instead of being copied to the card as it came
out of a camera:

  * H.264 constrained baseline (the profile the esp_h264 software decoder
    supports), Annex-B byte stream, yuv420p
  * 240x240, scaled up to fill the panel and center-cropped, 15 frames per
    second (--fit pad keeps the whole picture with black bars instead)
  * the sound as a separate MP3 track, mono, 48 kbit/s: the box has one small
    speaker, and a second channel costs bytes and decoder time for nothing

Video and sound go into one MPEG-TS file, so a movie is one file on the card
and the same file opens in VLC, mpv and ffmpeg without a conversion step. The
firmware's demuxer (src/movie/ts.c) reads the PES headers off and hands the
raw streams to the decoders; it also takes the frame rate from the timestamps
in those headers. Nothing is re-encoded for the container: both streams are
copied into the packets as they are.

The software decoder has the last word on the frame rate: 240x240 at 15 fps is
already close to its limit, and with a sound track next to it the picture
starts to stutter. The script warns when that combination is asked for.
--size 192 costs about a third of the pixels and is the other way out, as is
--no-audio for a silent movie.

usage: build_movies.py [-o OUTPUT_DIR] [--max-seconds N] [--fps N] [--crf N]
                       [--fit crop|pad] [--size N] [--no-audio]
                       INPUT [INPUT ...]

Needs ffmpeg and ffprobe on PATH (libx264 and libmp3lame enabled, which is the
default of every distribution package). Output names are the input name with
.ts instead of the original extension; the default output directory is
./movies in the project root.
"""

import argparse
import os
import subprocess
import sys
import tempfile

DEFAULT_SIZE = 240
MAX_SIZE = 240
MIN_SIZE = 64
SAMPLE_RATE = 44100
AUDIO_KBPS = 48
# The box has one small speaker; a second channel would only cost bytes.
AUDIO_CHANNELS = 1
DEFAULT_FPS = 15
DEFAULT_SECONDS = 120
MAX_FPS = 20  # above this the software decoder cannot keep up at 240x240
# With a sound track the picture starts to stutter above this rate at 240x240.
SAFE_FPS_WITH_AUDIO = 12

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd):
    """Runs a command and returns its stdout, raising on a failed exit."""
    proc = subprocess.run(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise RuntimeError("command failed: " + " ".join(cmd))
    return proc.stdout


def has_audio(path):
    out = run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "a",
            "-show_entries",
            "stream=index",
            "-of",
            "csv=p=0",
            path,
        ]
    )
    return bool(out.strip())


def encode_video(src, dst, fps, crf, max_seconds, fit, size):
    # crop scales until both sides cover the panel and cuts the overflow in
    # the middle, so the picture fills the round display; pad scales until it
    # fits and keeps black bars.
    if fit == "pad":
        geometry = (
            f"scale={size}:{size}:force_original_aspect_ratio=decrease:"
            "flags=lanczos,"
            f"pad={size}:{size}:(ow-iw)/2:(oh-ih)/2:color=black"
        )
    else:
        geometry = (
            f"scale={size}:{size}:force_original_aspect_ratio=increase:"
            "flags=lanczos,"
            f"crop={size}:{size}"
        )
    filters = f"{geometry},setsar=1,fps={fps},format=yuv420p"
    keyint = max(1, fps)
    x264 = (
        "cabac=0:ref=1:bframes=0:weightp=0:8x8dct=0:"
        f"keyint={keyint}:min-keyint={keyint}:scenecut=0"
    )
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src]
    if max_seconds > 0:
        cmd += ["-t", str(max_seconds)]
    cmd += [
        "-an",
        "-vf",
        filters,
        "-c:v",
        "libx264",
        "-preset",
        "medium",
        "-profile:v",
        "baseline",
        "-level",
        "3.0",
        "-x264-params",
        x264,
        "-crf",
        str(crf),
        "-pix_fmt",
        "yuv420p",
        "-f",
        "h264",
        dst,
    ]
    run(cmd)


def count_frames(path):
    out = run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-count_frames",
            "-show_entries",
            "stream=nb_read_frames",
            "-of",
            "csv=p=0",
            path,
        ]
    )
    return int(out.strip())


def encode_audio(src, dst, seconds):
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        src,
        "-vn",
        "-t",
        f"{seconds:.3f}",
        # apad fills a sound track that is shorter than the picture with
        # silence; -t cuts one that is longer.
        "-af",
        "apad",
        "-c:a",
        "libmp3lame",
        "-b:a",
        f"{AUDIO_KBPS}k",
        "-ar",
        str(SAMPLE_RATE),
        "-ac",
        str(AUDIO_CHANNELS),
        # Frames only: a Xing header only makes sense in a standalone file, and
        # the movie player takes its length from the picture.
        "-write_xing",
        "0",
        "-id3v2_version",
        "0",
        "-f",
        "mp3",
        dst,
    ]
    run(cmd)


def mux(video, audio, fps, dst):
    """Puts the two raw streams into one MPEG-TS file, without re-encoding.

    The muxer writes one PES per frame, so the firmware can take the frame
    duration from the timestamps; the picture and the sound are copied into
    the packets as they are.
    """
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-framerate",
        str(fps),
        "-f",
        "h264",
        "-i",
        video,
    ]
    if audio:
        cmd += ["-f", "mp3", "-i", audio]
    cmd += ["-map", "0:v:0"]
    if audio:
        cmd += ["-map", "1:a:0"]
    cmd += ["-c", "copy", "-f", "mpegts", dst]
    run(cmd)


def build_one(src, dst, fps, crf, max_seconds, fit, size, want_audio):
    with tempfile.TemporaryDirectory(prefix="moonstation-") as tmp:
        video_path = os.path.join(tmp, "video.h264")
        audio_path = os.path.join(tmp, "audio.mp3")

        encode_video(src, video_path, fps, crf, max_seconds, fit, size)
        frames = count_frames(video_path)
        if frames <= 0:
            raise RuntimeError("the input has no video frames")
        seconds = frames / fps

        audio = want_audio and has_audio(src)
        if audio:
            encode_audio(src, audio_path, seconds)
            audio = os.path.getsize(audio_path) > 0

        mux(video_path, audio_path if audio else None, fps, dst)

        video_bytes = os.path.getsize(video_path)
        audio_bytes = os.path.getsize(audio_path) if audio else 0

    return frames, video_bytes, audio_bytes


def main(argv):
    parser = argparse.ArgumentParser(
        description="Convert videos into .ts files for the moonstation "
        "movie mode."
    )
    parser.add_argument(
        "inputs", nargs="+", metavar="INPUT", help="source video file"
    )
    parser.add_argument(
        "-o",
        "--output",
        default=os.path.join(PROJECT_ROOT, "movies"),
        help="output directory (default: ./movies in the project root)",
    )
    parser.add_argument(
        "-t",
        "--max-seconds",
        type=int,
        default=DEFAULT_SECONDS,
        help=f"cut a movie after this many seconds, 0 keeps all of it "
        f"(default: {DEFAULT_SECONDS})",
    )
    parser.add_argument(
        "--fps",
        type=int,
        default=DEFAULT_FPS,
        help=f"frames per second, 1-{MAX_FPS} (default: {DEFAULT_FPS})",
    )
    parser.add_argument(
        "--crf",
        type=int,
        default=26,
        help="x264 quality, lower is better and bigger (default: 26)",
    )
    parser.add_argument(
        "--fit",
        choices=("crop", "pad"),
        default="crop",
        help="crop fills the panel and cuts the sides of a wider picture, "
        "pad keeps the whole picture with black bars (default: crop)",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=DEFAULT_SIZE,
        help=f"picture edge in pixels, even and {MIN_SIZE}-{MAX_SIZE}; 192 "
        f"decodes a third less than the panel can show (default: "
        f"{DEFAULT_SIZE})",
    )
    parser.add_argument(
        "--no-audio",
        action="store_true",
        help="leave the sound track out; the picture gets the full frame time",
    )
    args = parser.parse_args(argv)

    if not 1 <= args.fps <= MAX_FPS:
        parser.error(f"--fps must be between 1 and {MAX_FPS}")
    if not 0 <= args.crf <= 51:
        parser.error("--crf must be between 0 and 51")
    if args.max_seconds < 0:
        parser.error("--max-seconds must not be negative")
    if args.size % 2 or not MIN_SIZE <= args.size <= MAX_SIZE:
        parser.error(
            f"--size must be an even number from {MIN_SIZE} to {MAX_SIZE}"
        )

    if not args.no_audio and args.fps > SAFE_FPS_WITH_AUDIO:
        try:
            loud = [
                s
                for s in args.inputs
                if os.path.isfile(s) and has_audio(s)
            ]
        except (RuntimeError, ValueError):
            loud = []
        if loud:
            print(
                f"warning: {args.fps} fps with a sound track is more than the "
                f"decoder keeps up with at {args.size}x{args.size}; "
                f"--fps {SAFE_FPS_WITH_AUDIO} plays smoothly.\n"
                "recommendation: remove the audio channel (--no-audio), the "
                "picture then keeps the full frame time. The box has one small "
                "speaker, so a sound track costs picture smoothness.",
                file=sys.stderr,
            )

    os.makedirs(args.output, exist_ok=True)

    failed = 0
    for src in args.inputs:
        if not os.path.isfile(src):
            print(f"skipping {src}: not a file", file=sys.stderr)
            failed += 1
            continue
        name = os.path.splitext(os.path.basename(src))[0] + ".ts"
        dst = os.path.join(args.output, name)
        try:
            frames, video_bytes, audio_bytes = build_one(
                src,
                dst,
                args.fps,
                args.crf,
                args.max_seconds,
                args.fit,
                args.size,
                not args.no_audio,
            )
        except (RuntimeError, ValueError) as err:
            print(f"{src}: {err}", file=sys.stderr)
            failed += 1
            continue
        total = os.path.getsize(dst)
        length = frames / args.fps
        print(
            f"wrote {dst}: {length:.1f} s, {frames} frames, "
            f"{args.size}x{args.size}, {video_bytes // 1024} kB video, "
            f"{audio_bytes // 1024} kB audio, {total // 1024} kB total"
        )

    if failed:
        print(f"{failed} input(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
