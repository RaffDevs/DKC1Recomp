#!/usr/bin/env python3
"""Determinism gate for a newly accepted DKC1 ROM revision.

Runs the headless host three times from a cold boot with the same frame count
and checks that the final frame/WRAM/VRAM/CGRAM/OAM hashes are byte-identical.
No ROM, state, or generated code is copied or committed; a small JSON record
of the run is written to --output.

usage: python3 tools/validate_rev2.py --exe <headless> --rom <rom.sfc> \
           --frames 600 --output build/rev2-validate.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

HASH_KEYS = ("frame_sha256", "wram_sha256", "vram_sha256", "cgram_sha256",
             "oam_sha256", "oam_source_sha256")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run(exe: Path, rom: Path, frames: int) -> tuple[dict, str]:
    env = dict(os.environ)
    env["DKC1_WIDESCREEN"] = "0"
    env.pop("DKC1_STARTUP_SCRIPT", None)
    env.pop("SNESRECOMP_INPUT_PLAY", None)
    result = subprocess.run([str(exe), str(rom), str(frames)],
                            capture_output=True, text=True, env=env, timeout=900)
    if result.returncode != 0:
        raise RuntimeError(
            f"headless host exited {result.returncode}: "
            f"{result.stderr[-2000:]}")
    values = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if key in HASH_KEYS:
                values[key] = value.strip()
    missing = [key for key in HASH_KEYS if key not in values]
    if missing:
        raise RuntimeError(f"headless output missing hashes: {missing}")
    return values, result.stderr[-4000:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--output", type=Path,
                        default=Path("build/rev2-validate.json"))
    args = parser.parse_args()

    runs = []
    for index in range(3):
        values, stderr_tail = run(args.exe.resolve(), args.rom.resolve(),
                                  args.frames)
        runs.append({"index": index, "hashes": values,
                     "stderr_tail": stderr_tail})

    first = runs[0]["hashes"]
    for other in runs[1:]:
        if other["hashes"] != first:
            raise RuntimeError(
                "determinism gate FAILED: "
                f"run 0 {first} != run {other['index']} {other['hashes']}")

    record = {
        "gate": "rev2-determinism",
        "rom": {"name": args.rom.name, "sha256": sha256_file(args.rom)},
        "executable": str(args.exe),
        "frames": args.frames,
        "widescreen": False,
        "pass": True,
        "runs": runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2) + "\n",
                           encoding="utf-8")
    print(json.dumps({"pass": True, "output": str(args.output),
                      "hashes": first}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
