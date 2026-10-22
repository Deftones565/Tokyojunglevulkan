# The prior port: sp00nznet/tokyojungle

What the earlier Tokyo Jungle port learned, what's still open, and what carries
over to this one. Source: `third_party/tokyojungle-ref` at `5b7cc00`, mainly
`docs/WHERE_IT_STOPS.md` (2,069 lines, a dated lab notebook), `README.md`,
`docs/PROGRESS.md` and `docs/TECHNICAL.md`. Read 2026-10-14 by MELCHIOR-1.

## Read the newest notes, not the README

The README's status is stale and contradicts the notebook.

| | README ("Current Blocker") | `WHERE_IT_STOPS.md`, last sections |
|---|---|---|
| Boot | parks waiting on the game clock at `0x00C04A68` | clock fixed; the main loop runs |
| Rendering | "window opens, clears present, no geometry" | the title, menus and most screens render |
| Furthest point | main loop | main menu and 16 forced screens, ~30 fps |

The 0x00C04A68 stall and "no geometry" are **solved problems**. The causes are
recorded below because the same bugs can come back on our build.

## Where it actually got to

Windows, D3D12, v1.00 EBOOT, ps3recomp at roughly v0.5–0.10.

- **Boot to the main menu.** It runs title logo → "PRESS ANY BUTTON" → menu
  (SURVIVAL / STORY / STATS / ARCHIVES) at about 30 fps, which matches a reference
  recording of the same states.
- **16 screens forced and captured:** story select, results, ranking, stats,
  the movie player and autosave all draw finished UI.
- **Never reached:** gameplay (Survival or Story), audio output, real pad
  input, movie decode.
- **There's no attract mode in this binary.** It has no idle counter, the
  any-button check has one caller, and no `attract` string exists. Waiting at
  the title won't lead anywhere, so don't spend time on it.

## The 0x00C04A68 counter stall (resolved): a chain of six bugs

`0x00C04A68` is the game clock at `[[TOC+0x1860]+0x18]` (v1.00 addresses). The
main thread waits in `func_00213A14` until it advances, and **only the audio
thread `sgx-audio-thr` advances it.** So any failure in audio bring-up shows up
as "the counter is never written". Fixing it took this chain, in order:

1. **`/dev_hdd0` didn't resolve.** A file request failed, so its completion
   never decremented `[0x024F4A50]`, so the main thread sat in `sys_cond_wait`
   for good. Fixed by mapping `/dev_hdd0/game/NPUA80523/USRDIR` onto the game
   data.
2. **`cellSpursShutdownJobChain` was unimplemented** and returned whatever was in r3.
3. **`cellSpursFinalize` cancelled every SPURS queue.** The title runs one SPURS
   for data install and another for audio, so finalising the first silenced the
   second (69 × `failed to recv data from SPU`).
4. **`cellSail` checked guest pointers as indices**, so `SetSoundAdapter` failed
   and SGX tore itself down.
5. **`sys_ppu_thread_once` was unimplemented.** It fell through to a
   return-`CELL_OK` stub, so SPURS bring-up was skipped and a later `fread` went
   into NULL (ps3recomp #136). It needs blocking semantics: a second caller has
   to wait.
6. **One SPU workload was a dispatch MISS** (`fp=0x78FEA84E1C23BB5C`). It's the
   job that sets the event flag the boot waits on. Lifting it gave the first draw.

Runtime fixes 2–5 are in our pinned ps3recomp (checked in its git log):

- 2 and 3: `6560f40`
- 4: `ceb7e63` and #190
- 5: #136

The SPURS query answer (#135) and the FIFO resync that swallowed fences (#137)
are in too. #133 (cellSail completion callbacks) and #134 (write-watch) don't
show up by PR number, so if those symptoms come back, check those first.

Fix 1 is title-side and handled by `src/main.cpp`'s `setup_vfs()`. Fix 6 is
per-build data; see "SPU images" below.

## Open blockers carried over

### SPU images: raw blobs, but inside the EBOOT

- Tokyo Jungle's SPURS jobs are **raw code+data blobs**, not SPU ELFs, so
  `extract_spu_images.py` finds nothing. (Correction, 2026-10-22: they are not
  loaded from the data files. All 18 sit byte for byte in the decrypted 1.01
  EBOOT, and `scripts/extract_spu_jobs.py` cuts them out; the capture below is
  how they were first found.)
- The runtime matches each one by FNV-1a-64 content fingerprint. With no match it
  logs `[spurs-job] dispatch MISS fp=… size=…`, **runs nothing**, and the title
  waits forever on work that never happens.
- The prior port captured all 12 images with `SPU_DUMP_MISS`, lifted them
  (4,751 SPU functions) and registered them in `src/gen/spu_workloads.c`.
  **Only that registry is in the ref repo. The dumps and lifted sources aren't.**
- What we need to do:
  1. Run with `SPU_DUMP_MISS`.
  2. For each dump: `find_spu_functions.py --raw --base 0 --out funcs.json <dump>`,
     then `spu_lifter.py --base 0 --functions funcs.json --symbol-prefix <p> <dump>`
     into `generated/spu/<image>/`.
  3. Regenerate the registry (`build_spu_workloads.py`).
  4. Repeat until there are zero misses.

  `CMakeLists.txt` already picks up `generated/spu/*/spu_recomp.c`.
- **Fingerprints are tied to the data files.** If v1.01 changed any job blob,
  the ref's fingerprint list won't match, so don't copy its registry.

### cellFont: no UI text through the font library

- The prior port kept `cellFont*` on its own stubs (`TJ_KEEP='cellFont*'`), so
  nothing it rendered came through cellFont. Its menu text is pre-rendered
  texture text from `strData_EN.pac` and friends, which is why menus look
  complete anyway.
- Upstream now lists cellFont and cellFontFT as **Complete** (stb_truetype backend).
- **The open question is whether the title needs system fonts from
  `/dev_flash/data/font/`.** Our runner serves `/dev_flash` from `$PS3_DEV_FLASH`,
  or an empty directory if that's unset. With no fonts we should expect cellFont
  to fail to open them. Test with a real dev_flash before touching cellFont itself.

### Others

- **An intermittent early-boot audio race** fails about 2 runs in 8:
  `sgxbus.c(934): failed to regist module`. One of eight DSP module registrations
  gets back a short reply count (e.g. `want=476 got=284`), and which module varies.
  The SPU answers are byte-identical between passing and failing runs, so the
  bug is downstream of them. The next probe would be the two `func_00201770`
  call sites in `func_00201D38`. **Judge any boot over several runs**: three
  failures in a row from a 50% race look exactly like a deterministic bug.
- **No audio output.** cellAudio initialises and the SPU jobs run, but nothing
  is wired to a host device.
- **No pad input.** The title-to-menu step was forced with debug knobs.
- **cellSail `OpenStream` is a stub**, so no movies play. URIs resolve, e.g.
  `/dev_hdd0/game/NPUA80523/USRDIR/data/region/US/load_credit_US.pam` (PAMF).
- **Unhandled RSX methods** `0x1D88`, `0x1800`, `0x0398`, `0x0394`, `0x01B8` and
  `0x01B4` were logged early on. That was before rendering worked, so recheck them.

## Things that will bite us on Linux/Vulkan

- **Nobody has booted this title on Linux.** The prior port was Windows-only,
  and upstream says no game has booted on Linux from its tree (see
  `docs/TOOLCHAIN.md`). Our runner does pass ps3recomp's boot smoke title on
  Vulkan (all 11 markers).
- **We lack the prior port's capture and debug tools:**
  - `CELLMARK_DUMP`, the only frame capture the prior port trusted (its
    screenshots showed only the sky layer), exists only in the D3D12 backend.
    **Our runner's `TJ_FRAME_DUMP` replaces it on Vulkan** (numbered PPMs; see
    `src/main.cpp`). It was checked against the smoke title's own clear colour.
  - `TJ_FASTTITLE`, `TJ_FORCEBTN` and `TJ_GOTO` are in neither repo. They
    weren't published, so there's no screen forcing or fake button presses.
  - `TJ_TICK_GAMECLOCK` lived in the ref's `src/tj_present.cpp`. We didn't port it.
- **Logging changes timing.** Measure with `PS3_VERBOSE=0`.
  `ps3_log_verbose()` treats a non-tty stderr as "be verbose", so redirecting to
  a file turns full logging on, which starves threads on the stdio lock.
- **guest argv sits inside the game's BSS.** `ppu_run` writes argv at guest
  `0x00B00000`, and v1.00's BSS spans `0x3BED80`–`0xC0E208`. The prior port
  used `0x00F00000` with its own boot code. Check v1.01's layout. If BSS still
  covers `0xB00000`, the strings land in live game data.
- **The `/dev_flash` default is a Windows path** (`D:/recomp/tools/rpcs3/dev_flash`)
  in ppu_fs. `src/main.cpp` always sets `PS3_DEV_FLASH`, so we avoid it.

## What carried over from the prior port

| Prior port | Here | Why |
|---|---|---|
| `src/main.cpp` (own boot, Win32) | `src/main.cpp` from ps3recomp's template | The scaffold now does ELF load, TLS, argv, import binding and threads. The ref's hand-rolled boot predates it and is Win32 throughout (VEH, `VirtualAlloc`, `__try`). |
| VFS root + cellFs mappings | `setup_vfs()` | Same intent, with one PS3-shaped tree that both filesystem layers agree on |
| `argv[0] = /app_home/USRDIR/EBOOT.BIN` | `PS3_BOOT_PATH` default, changed to `/dev_hdd0/game/NPUA80523/USRDIR/EBOOT.BIN` | Without an argv the title built no content path. The RPCS3 oracle shows the real game gets the hdd path |
| `src/stubs.cpp` | not carried | Its `register_overrides()` was a TODO; none of it was ever active |
| `hle_imports.h`, `import_resolver.h` | not carried | Import slots hardcoded for v1.00 (`0x342810`, …); replaced by `ppu_lifter.py --hle-stubs` |
| `scripts/analyze.py` | `scripts/analyze.py`, rewritten | Drives ppu_loader → find_functions → ppu_lifter |
| `gen_stubs.py`, `post_lift.py`, `fix_*.py` | not carried | They patch an older lifter's output and hardcode v1.00 (TOC `0x00359220`, CRT bodies) |
| `src/gen/spu_workloads.c` | not carried | Must be regenerated from our own dumps |
| `config/tokyojungle.toml` | `config/tokyojungle.toml`, rewritten | Paths, v1.01, Vulkan |

**Every guest address in this document is v1.00.** The v1.01 EBOOT is a
different build, so treat them as a guide to what to look for, not where.
