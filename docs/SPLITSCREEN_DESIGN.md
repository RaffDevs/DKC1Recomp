# Split-screen co-op design (Rev 2 fork)

Status: **Phase 1+2 implemented** (two sessions + compositor + shared startup
route). See "Implemented shape" below. Still unvalidated by visible playtest.

## Implemented shape (2026-11-17)

- `dkc1_snesrecomp_headless --split-server <port> <rom.sfc>` streams raw
  256x224 RGBA frames over loopback TCP. One input mask in, one frame out;
  `0xFFFFFFFF` shuts it down. No ROM/save/generated data crosses the socket.
- `dkc1_dual_host` forks two of those servers, connects to both, sends the
  same input mask to each, waits for both frames and presents them top/bottom
  with a divider. It links no emulator code and owns no guest state.
- `--startup <masks.txt>` sends a deterministic input stream to **both**
  sessions before live input takes over. `recipes/one_player_jungle_playback.txt`
  is an `DKC1_INPUT_RECORD` capture of `recipes/route_jungle.dks`, so both
  sessions boot into 1P Jungle Hijinxs together.
- Run it with `./scripts/run_split.sh <rom>`. P1: arrows + S/Z/A/X/Q/W/
  Return/RShift or gamepad 0. P2: IJKL + U/O/P/;/9/0/Backslash/RCtrl or
  gamepad 1. Escape or window close stops both sessions.

**Important scope note:** each session is a separate 1P cartridge run. That is
what gives genuinely independent cameras. The cartridge's built-in "2 Players
Team" mode shares one world and one camera, so it cannot be split without
duplicating guest state — a much larger project. Each player currently has
their own copy of the level; shared enemies/inventory are not implemented.

## Goal

Local split vertical: two DKC players, each with an independent camera and a
full 256x224 native view, shown in the top and bottom halves of one window.
Each player controls their own Donkey/Diddy pair through a separate level.

Non-goals for the first milestone: online netplay, rollback, shared inventory,
one-monster-at-a-time "follow the leader" co-op, or a single shared camera.

## Why two sessions, not one

- The runner is a single-machine design: `g_snes`, `g_ppu`, `g_snes_cpu`,
  `g_cpu`, `g_ram` are process globals. The DKC adapter adds statics
  (`s_resume_pc`, `s_next_frame_master`, `s_cpu_initialized`).
- One PPU holds one VRAM/OAM/scroll state and draws one 256x224 image. DKC's
  camera and player objects live in one WRAM. Two cameras therefore need two
  machines, not two viewport rectangles.
- Refactoring the engine into reentrant contexts is possible, but it is a large
  game-agnostic change with a heavy validation burden across every consumer.
  It is not on the critical path for this title.
- Engine precedent: the shared netplay notes disable widescreen for local
  split-screen (Metal Warriors H2H). Keep `DKC1_WIDESCREEN=0` in split mode.

## Architecture

```
   pad 1 --> +---------------------------+
             | session A (instance 0)    | --frame A--+
             | ROM, WRAM, VRAM, OAM, APU |            |
             +---------------------------+            |  +------------------+
                                                     +->| compositor host  |
   pad 2 --> +---------------------------+            |  | top = A          |
             | session B (instance 1)    | --frame B--+  | bottom = B       |
             | ROM, WRAM, VRAM, OAM, APU |               | divider, audio   |
             +---------------------------+               +------------------+
```

- Each session is the existing deterministic headless host
  (`runner/headless_main.c`) with a new frame sink. The seam already exists:
  `Dkc1BeginDrawing(uint8_t *pixels, size_t pitch)` accepts a caller-owned
  buffer, and `Dkc1DrawPpuFrame()` renders into it.
- Sessions advance **lockstep**: the compositor requests frame N from both,
  waits for both, then presents. This makes both input streams reproducible
  and keeps A/B evidence byte-comparable.
- Input: compositor owns SDL/controller polling and writes the per-slot joypad
  words into each session before stepping frame N.
- Audio: mix both S-DSP streams, or P1 only for the first milestone (document
  the limitation explicitly).
- Saves: per-session namespaces (`saves/p1`, `saves/p2`). Never share an SRAM
  file between sessions.
- Session start: both sessions cold-boot the verified Rev 2 ROM and load the
  same save slot, so both enter the same level. A host-level "Start co-op"
  command releases both from file select together. No DKC game-code change is
  needed for the stock flow.

## Phases

1. **Offscreen frame export.** Add a frame sink to the headless host (local
   socket or shared memory) plus a `--headless-split` launch mode. Verify one
   session can stream 256x224 ARGB frames deterministically.
2. **Compositor host.** Two sessions, two input slots, lockstep clock, vertical
   split with a divider. Target `dkc1_dual_host` in `CMakeLists.txt`. This is
   the first playable milestone.
3. **Session coordination.** Shared pause/resume, synchronized level start,
   per-player save slots, and a documented death/respawn policy.
4. **Optional gameplay coupling** (all default-off): shared lives/bananas,
   revive, or distance rubber-banding. These change game policy and need their
   own evidence; do not enable them silently.
5. **Validation.** Three deterministic two-input replays from the same roots;
   each player's native 256-pixel center compared against a trusted emulator;
   host CPU cost measured per `docs/PERFORMANCE.md`; no claim of full-campaign
   certification without the project's gates.

## Files

| Area | File(s) |
| --- | --- |
| Frame sink | `runner/headless_main.c` (`Dkc1SplitServer`) |
| Compositor | `runner/dual/dual_host.c`, `CMakeLists.txt`, `scripts/run_split.sh` |
| Startup route | `recipes/one_player_jungle_playback.txt` (generated, no ROM bytes) |
| Docs | this file, `docs/ROM_REVISIONS.md`, `README.md` status line |

Audio is **not forwarded** yet: the frame protocol carries video only, so the
split window is silent. Mixing the two S-DSP streams is the next step.

## Open questions

- Should a player who dies restart their own session only, or wait for both?
- Is separate audio mixing worth the CPU cost on Apple Silicon, or is P1-only
  audio acceptable for the first release?
- Do both players need the same level, or is independent map progression a
  feature? (Independent is simpler: each session keeps its own SRAM.)
