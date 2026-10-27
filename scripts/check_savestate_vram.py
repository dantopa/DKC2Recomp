#!/usr/bin/env python3
"""Private route: a save inside a multi-frame VRAM upload resumes it in place.

The boot route uploads the first level's graphics through the PPU ports over
several frames around frame 2600. A state saved there and loaded must reach
the same VRAM at the end of the route as the uninterrupted run.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from check_coop_route import TEAM_START, check

SAVE_FRAMES = (2600, 2800)


def run(runner, rom, directory, name, rows, **extra):
    inputs, vram = directory / f"{name}.input", directory / f"{name}.vram"
    inputs.write_text("".join(f"{word:06x}*1\n" for word in rows), encoding="ascii")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("DKC2_", "SNESRECOMP_"))}
    env.update(SNESRECOMP_INPUT_PLAY=str(inputs), DKC2_VRAM_OUTPUT=str(vram), **extra)
    result = subprocess.run([str(runner), str(rom), str(len(rows))], cwd=directory,
                            env=env, capture_output=True, text=True, timeout=90)
    check(result.returncode == 0, result.stdout + result.stderr)
    return vram.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    args = parser.parse_args()
    runner, rom = args.runner.resolve(strict=True), args.rom.resolve(strict=True)
    rows = [word for word, duration in TEAM_START for _ in range(duration)]
    with tempfile.TemporaryDirectory(prefix="dkc2-savestate-vram-") as tmp:
        directory = Path(tmp)
        expected = run(runner, rom, directory, "continuous", rows)
        for frame in SAVE_FRAMES:
            state = directory / f"{frame}.sav"
            run(runner, rom, directory, f"save{frame}", rows[:frame],
                DKC2_SAVESTATE_OUTPUT=str(state))
            resumed = run(runner, rom, directory, f"load{frame}", rows[frame:],
                          DKC2_SAVESTATE_INPUT=str(state))
            differing = sum(a != b for a, b in zip(resumed, expected))
            check(differing == 0,
                  f"state saved at frame {frame}: {differing} VRAM bytes differ")
    print("Savestate VRAM passed: saves during the level upload resume it in place")


if __name__ == "__main__":
    main()
