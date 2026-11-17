#!/usr/bin/env bash
# Apply the local, game-agnostic engine patches required to analyze Rev 2.
# Idempotent: safe to run after every `git submodule update --init --recursive`.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
patch_dir="$repo_dir/patches"
submodule="$repo_dir/snesrecomp"

if [[ ! -d "$submodule/.git" && ! -f "$submodule/.git" ]]; then
  echo "apply_engine_patches: snesrecomp submodule is not initialized" >&2
  exit 2
fi

for patch in "$patch_dir"/*.patch; do
  [[ -e "$patch" ]] || continue
  if git -C "$submodule" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "engine patch already applied: $(basename "$patch")"
    continue
  fi
  git -C "$submodule" apply "$patch"
  echo "applied engine patch: $(basename "$patch")"
done
