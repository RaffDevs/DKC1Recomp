# Supported ROM revisions

The framework originally pinned a single identity: headerless *Donkey Kong
Country* USA v1.0, SHA-256 `fa8cacf5…f74d15`. This fork also accepts the USA
v1.2 / "Rev 2" dump, SHA-256 `6281474e…5f0527` (CRC32 `762af827`, internal
CRC `2BCC`). Both are 4 MiB HiROM with 2 KiB battery SRAM and no coprocessor.

## What was changed

| File | Change |
| --- | --- |
| `runner/verified_rom.c` | `kSupportedSha256s[]` holds both revisions; `IsSupportedRom()` accepts either. The v1.0-only `DKC1_ALLOW_ROM_SHA256` development override is unchanged. |
| `runner/verified_rom.h` | Document both accepted payloads. |
| `scripts/generate_snesrecomp.py` | `EXPECTED_ROMS` accepts v1.2 as a stock generation input. |
| `recomp/symbols.toml` | Records both revisions; generation target is v1.2. |

### Two additional generation facts

1. **Analyzer bounds fix (in the pinned `snesrecomp` submodule).** Rev 2
   changes the bytes at `BF:B1E5` so the v1.0-derived cfg root decodes a
   `JSR $FF49` into compressed graphics; the instruction at `BF:FFFF` then
   read one byte past the 4 MiB image and both analyzers crashed. The local
   submodule patch makes `decode_insn()` return `None` when an instruction or
   its operands do not fit in the image. This is game-agnostic fail-closed
   behavior and must be upstreamed before the submodule pin is updated.
2. **The native NMI vector moved.** v1.0 points at `$A968`; the Rev 2 header
   declares `$A976`. The adapter used to hardcode the v1.0 address, so Rev 2
   ran the wrong handler: the CPU advanced through boot modes but never turned
   the PPU/APU on (0 video and 0 audio frames over a 3600-frame soak).
   `Dkc1ReadVerifiedRom()` now reads `$00:FFEA` from the accepted ROM and
   `Dkc1NativeNmiPc()` supplies it to the frame driver. This is revision
   detection, not a heuristic, and remains correct for v1.0.
3. **Widescreen overrides are v1.0-only.** The override pass fails on Rev 2
   (`expected exactly one generated unit defining CODE_809E32_M0X0; found 0`).
   Generate Rev 2 with `--no-widescreen-overrides` until those anchors are
   re-derived from the v1.2 bytes. The first playable milestone is therefore
   the authentic 256x224 presentation, not widescreen.

`tools/gen_replacements.py`, `tools/bisect_transition_contamination.py`,
`tools/promote_bundle.py` and the validation documents still pin v1.0 on
purpose: they bless byte ranges or historical evidence produced from v1.0 and
must fail closed until someone re-blesses them against v1.2.

## Why Rev 2 needs validation, not just a hash

The `recomp/*.cfg` function boundaries, M/X entry states and indirect-dispatch
target lists were derived from the byte-exact v1.0 disassembly. Public version
differences (TCRF, Donkey Kong Wiki) list v1.2 bugfixes that touch gameplay
code, notably:

- Mini-Necky / Necky projectile collision;
- mine-cart jump and air-roll fixes in Mine Cart Carnage and Mine Cart Madness;
- the Kongo Jungle corner warp;
- the first-level fanfare no longer being skippable;
- two Slip Slide Ride fixes.

These are believed to be small in-place patches against the v1.0 layout, but
that is a hypothesis until it is checked byte-for-byte. A shifted function or a
new indirect target would make the v1.0-derived cfg incomplete for v1.2.

## Bring-up gate for the new revision

1. Generate C from the v1.2 ROM with `scripts/generate_snesrecomp.py` and keep
   the output in ignored `generated/snesrecomp/`.
2. Build the desktop and headless hosts on macOS (`./build_macos.sh <rom>`),
   then run `python3 tools/validate_rev2.py --exe build/macos/dkc1_snesrecomp_headless
   --rom <rom> --frames 600 --output build/rev2-validate.json` for the
   three-replay determinism gate.
3. Compare a v1.2 attract/route trace against a trusted emulator for the same
   dump. The native 256-pixel center is the oracle; do not use the v1.0
   widescreen evidence as a substitute.
4. Walk the scenes named in the version-difference list above, especially the
   mine-cart and Mini-Necky cases, and record what passed and what did not.
5. Only then promote v1.2 from "accepted by the verifier" to "validated port".

## Recorded result (2026-11-17, this fork)

- Generated with `--no-widescreen-overrides`: 2567 roots, 3058 AOT variants,
  1067 LLE variants; 14 bank units emitted.
- Built `dkc1_macos` and `dkc1_snesrecomp_headless` with `./build_macos.sh`.
- `tools/validate_rev2.py --frames 600`: three cold-boot replays byte-identical
  (`frame_sha256=a7366146…b50430dd`, `wram_sha256=f8b3bce5…dafd236`,
  `vram_sha256=628205f6…b5386b6a`).
- 3600-frame soak: 3481 video-active frames, 3222 audio-active frames,
  3,419,452 nonzero audio samples, no runtime/LLE failure, exit 0.
- Frame 600 shows the correct Nintendo "Presents" splash (256x224, 4:3).

This is **boot-and-attract validated**, not a claim of full-game certification.
The v1.2 bugfix scenes and a reference-emulator comparison are still open.
