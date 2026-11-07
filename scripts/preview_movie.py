#!/usr/bin/env python3
"""Checks and plays a movie of the movie mode on the desktop.

A movie is MPEG-TS, what scripts/build_movies.py writes. It is a standard
container, so VLC, mpv and ffmpeg open the file on the card as it is; this tool
prints what is inside, checks it against what the box can decode, and then
plays it with ffplay.

usage: preview_movie.py [-o OUTPUT] MOVIE [MOVIE ...]

Without -o the movie is played and nothing is kept. With -o the file is written
instead: a .ts is copied, any other container is remuxed without re-encoding
(.mkv is the safe choice, .mp4 only takes MP3 audio in recent players). One
movie per -o run.

Needs ffmpeg and ffprobe on PATH, plus ffplay for playback.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from fractions import Fraction

# What the panel and the software decoder can do; a file outside this would not
# play on the box either.
MAX_SIDE = 240
MAX_FPS = 20


def capture(cmd):
    """Runs a command and returns its stdout as text."""
    proc = subprocess.run(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise RuntimeError("command failed: " + " ".join(cmd))
    return proc.stdout


def run(cmd):
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError("command failed: " + " ".join(cmd))


def probe_ts(path):
    """Reads the streams of a transport stream and checks them for the box."""
    out = capture(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "stream=codec_type,codec_name,profile,width,height,r_frame_rate"
            ":format=duration",
            "-of",
            "json",
            path,
        ]
    )
    data = json.loads(out)
    video = None
    audio = None
    for stream in data.get("streams", []):
        if stream.get("codec_type") == "video" and video is None:
            video = stream
        elif stream.get("codec_type") == "audio" and audio is None:
            audio = stream
    if video is None:
        raise ValueError("no video stream")

    if video.get("codec_name") != "h264":
        raise ValueError(
            f"video is {video.get('codec_name')}, the box only decodes H.264"
        )
    width = int(video.get("width") or 0)
    height = int(video.get("height") or 0)
    if not 16 <= width <= MAX_SIDE or not 16 <= height <= MAX_SIDE:
        raise ValueError(f"{width}x{height} does not fit the 240x240 panel")
    rate = Fraction(video.get("r_frame_rate") or "0/1")
    fps = float(rate) if rate else 0.0
    if fps <= 0 or fps > MAX_FPS + 0.01:
        raise ValueError(f"{fps:.2f} fps is outside the 20 fps the box keeps up "
                         "with")

    duration = float(data.get("format", {}).get("duration") or 0)
    return {
        "width": width,
        "height": height,
        "fps": fps,
        "duration": duration,
        "profile": video.get("profile", "?"),
        "audio": audio.get("codec_name") if audio else None,
    }


def print_ts_info(name, info):
    print(
        f"{name}: MPEG-TS, {info['width']}x{info['height']}, "
        f"{info['fps']:.2f} fps, {info['duration']:.1f} s"
    )
    print(
        f"  video H.264 {info['profile']}, audio {info['audio'] or 'none'}, "
        f"file {os.path.getsize(name) // 1024} kB"
    )


def play(path):
    run(
        [
            "ffplay",
            "-loglevel",
            "error",
            "-autoexit",
            "-window_title",
            os.path.basename(path),
            path,
        ]
    )


def main(argv):
    parser = argparse.ArgumentParser(
        description="Check and play a movie of the movie mode (.ts)."
    )
    parser.add_argument("movies", nargs="+", metavar="MOVIE", help=".ts file")
    parser.add_argument(
        "-o",
        "--output",
        help="write the movie here instead of playing it (.mkv is the safe "
        "choice)",
    )
    args = parser.parse_args(argv)

    if args.output and len(args.movies) > 1:
        parser.error("-o takes one movie at a time")

    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        print("ffmpeg and ffprobe have to be on PATH", file=sys.stderr)
        return 1
    if not args.output and not shutil.which("ffplay"):
        print(
            "ffplay is not on PATH, use -o to write a file instead",
            file=sys.stderr,
        )
        return 1

    failed = 0
    for path in args.movies:
        try:
            info = probe_ts(path)
            print_ts_info(path, info)
            if args.output:
                # A copy is all a .ts needs; any other container takes a remux
                # without re-encoding.
                if os.path.splitext(args.output)[1] == ".ts":
                    shutil.copyfile(path, args.output)
                else:
                    run(
                        [
                            "ffmpeg",
                            "-hide_banner",
                            "-loglevel",
                            "error",
                            "-y",
                            "-i",
                            path,
                            "-map",
                            "0",
                            "-c",
                            "copy",
                            args.output,
                        ]
                    )
                print(f"wrote {args.output}")
            else:
                play(path)
        except (OSError, RuntimeError, ValueError) as err:
            print(f"{path}: {err}", file=sys.stderr)
            failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
