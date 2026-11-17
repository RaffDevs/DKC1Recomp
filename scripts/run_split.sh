#!/usr/bin/env bash
# Build and launch the two-session split-screen host.
# P1: arrows + S/Z/A/X/Q/W/Return/RShift, or gamepad 0.
# P2: IJKL + U/O/P/; / 9/0/Backslash/RCtrl, or gamepad 1.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${DKC1_BUILD_DIR:-$repo_dir/build/macos}"
rom="${1:-$DKC1_SPLIT_ROM}"

if [[ -z "$rom" || ! -f "$rom" ]]; then
  echo "usage: $0 <rom.sfc>   (or set DKC1_SPLIT_ROM)" >&2
  exit 2
fi

cmake --build "$build_dir" --target dkc1_dual_host dkc1_snesrecomp_headless \
  --parallel

exec "$build_dir/dkc1_dual_host" \
  --rom "$rom" \
  --headless "$build_dir/dkc1_snesrecomp_headless" \
  --startup "$repo_dir/recipes/one_player_jungle_playback.txt" \
  --scale "${DKC1_SPLIT_SCALE:-2}"
