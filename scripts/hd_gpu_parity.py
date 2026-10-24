#!/usr/bin/env python3
"""Checks the GPU HD compositor against the CPU one, and both against golden
captures when a private golden directory is given.

usage: hd_gpu_parity.py --app APP --rom ROM --input ROUTE --work DIR
                        [--pack DIR] [--golden DIR [--update-golden]]
                        [--frames 1300,1370,1470,1500]

The desktop app replays ROUTE hidden, once with DKC2_HD_CPU=1 and once on the
GPU, at 4x smooth with nearest presentation, and captures each frame. Golden
captures are game images: keep them outside the repository.
"""
import argparse
import os
import shutil
import subprocess
import sys

# Per channel, a pixel may differ by this much (rounding between executors).
CHANNEL_TOLERANCE = 2
# Pixels beyond CHANNEL_TOLERANCE allowed per capture, as a fraction.
OUTLIER_FRACTION = 1e-4
# No pixel may differ by more than this in any channel.
MAX_DIFFERENCE = 16
DEFAULT_FRAMES = "1300,1370,1470,1500"


def read_ppm(path):
    with open(path, "rb") as stream:
        data = stream.read()
    magic, width, height, maxval, pixels = data.split(maxsplit=4)
    if magic != b"P6" or maxval != b"255":
        raise ValueError(f"{path}: not an 8-bit binary PPM")
    width, height = int(width), int(height)
    return width, height, pixels[:width * height * 3]


def compare(first, second):
    """Returns (pixels beyond CHANNEL_TOLERANCE, largest channel difference)
    for two (width, height, pixels) images of the same size."""
    if first[:2] != second[:2]:
        raise ValueError(f"size {first[:2]} differs from {second[:2]}")
    a, b = first[2], second[2]
    over = largest = 0
    for i in range(0, len(a), 3):
        difference = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]),
                         abs(a[i + 2] - b[i + 2]))
        if difference > CHANNEL_TOLERANCE:
            over += 1
        largest = max(largest, difference)
    return over, largest


def within_tolerance(over, largest, width, height):
    return (largest <= MAX_DIFFERENCE and
            over <= int(width * height * OUTLIER_FRACTION))


def capture(args, mode, frames):
    """Runs the app in mode "cpu" or "gpu"; returns {frame: path}."""
    user = os.path.join(args.work, mode)
    shutil.rmtree(user, ignore_errors=True)
    os.makedirs(user)
    shot = os.path.join(user, "shot.ppm")
    env = dict(os.environ,
               DKC2_USER_DIR=user,
               DKC2_DESKTOP_TEST_FRAMES=str(max(frames) + 2),
               DKC2_DESKTOP_TEST_HIDDEN="1",
               DKC2_DESKTOP_DISABLE_SRAM="1",
               SNESRECOMP_NO_LAUNCHER="1",
               SNESRECOMP_INPUT_PLAY=args.input,
               DKC2_HD="4",
               DKC2_HD_FILTER="smooth",
               DKC2_ASPECT="16:9",
               DKC2_UPSCALER="nearest",
               DKC2_DESKTOP_SCREENSHOT=shot,
               DKC2_DESKTOP_SCREENSHOT_FRAME=",".join(map(str, frames)),
               DKC2_HD_CPU="1" if mode == "cpu" else "0")
    env.pop("DKC2_HD_PACK", None)
    if args.pack:
        env["DKC2_HD_PACK"] = args.pack
    result = subprocess.run([args.app, args.rom], env=env, capture_output=True,
                            text=True, check=False)
    if f"HD composition: {mode}" not in result.stdout:
        raise RuntimeError(f"{mode} run did not compose on the {mode}:\n"
                           f"{result.stdout}{result.stderr}")
    paths = {}
    for frame in frames:
        path = os.path.join(user, f"shot-{frame}.ppm")
        if not os.path.exists(path):
            raise RuntimeError(f"{mode} run wrote no capture for frame {frame}:"
                               f"\n{result.stdout}{result.stderr}")
        paths[frame] = path
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--pack")
    parser.add_argument("--golden")
    parser.add_argument("--update-golden", action="store_true")
    parser.add_argument("--frames", default=DEFAULT_FRAMES)
    args = parser.parse_args()
    frames = [int(frame) for frame in args.frames.split(",")]
    if len(frames) > 8:
        parser.error("at most 8 frames")
    if args.update_golden and not args.golden:
        parser.error("--update-golden needs --golden")

    shots = {mode: capture(args, mode, frames) for mode in ("cpu", "gpu")}
    failed = False
    for frame in frames:
        cpu = read_ppm(shots["cpu"][frame])
        checks = [("gpu/cpu", read_ppm(shots["gpu"][frame]), cpu)]
        if args.golden:
            golden = os.path.join(args.golden, f"hd-{frame}.ppm")
            if args.update_golden:
                os.makedirs(args.golden, exist_ok=True)
                shutil.copy(shots["cpu"][frame], golden)
            else:
                reference = read_ppm(golden)
                checks.append(("cpu/golden", cpu, reference))
                checks.append(("gpu/golden", checks[0][1], reference))
        for name, first, second in checks:
            over, largest = compare(first, second)
            ok = within_tolerance(over, largest, first[0], first[1])
            failed |= not ok
            print(f"frame {frame} {name}: {over} pixels beyond "
                  f"{CHANNEL_TOLERANCE}, max {largest} "
                  f"{'ok' if ok else 'FAILED'}")
    print("hd gpu parity:", "failed" if failed else "passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
