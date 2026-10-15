# The RPCS3 oracle

RPCS3 runs the real, unmodified game. When the recomp misbehaves, run the same
title under RPCS3 and compare. Its TTY log shows what the game itself prints
when it works, and its emulator log shows which firmware calls the game made
and what they returned. The prior port went 0 for 6 on one blocker by
inferring the answer instead of checking it against a working run.
(`docs/PRIOR_PORT.md`).

| | |
|---|---|
| `run_rpcs3.sh` | Boot the game in RPCS3 headless for N seconds and keep `TTY.log`, `RPCS3.log` and `run.txt` |
| `tty_diff.py` | Find the first line of RPCS3's TTY output that the recomp never prints |

## Setup (once)

1. **RPCS3:** the flatpak, `flatpak install --user flathub net.rpcs3.RPCS3`.
   Tested with 0.0.43-20251.
2. **Firmware.** `input/PS3UPDAT.PUP` is 4.93, downloaded from Sony's update
   server (`ps3-updatelist.txt`, `dus01.ps3.update.playstation.net`). Install it:

   ```sh
   flatpak run --filesystem="$PWD/input:ro" net.rpcs3.RPCS3 --installfw "$PWD/input/PS3UPDAT.PUP"
   ```

   - **Don't add `--headless`.** That combination aborts (exit 134) and installs nothing.
   - The window stays open when it's done. Close it.
   - It's installed when `~/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash/vsh/etc/version.txt`
     reads `release:04.9300`.
   - The recomp's runner uses this same `dev_flash` for system fonts.
3. **The game:** `tools/extract/setup_game.sh` installs 1.00 and the 1.01 update
   into RPCS3's `dev_hdd0/game/NPUA80523`, using the licence from
   `input/UP9000-NPUA80523_00-TOKYOJUNGLE00001.rap`.
   **Without that licence RPCS3 can't run the game.** It stops with
   `Failed to decrypt content`.

   A decrypted `EBOOT.ELF` doesn't avoid this. The game's DRM thread also opens
   `USRDIR/data/tj.edat`, which is encrypted too. The prior port's oracle run
   died exactly there, and in its case the extractor had dropped the file.

## Use

```sh
tools/oracle/run_rpcs3.sh -s 90                  # 90 s headless -> build-oracle/<timestamp>/
tools/oracle/run_rpcs3.sh -s 90 -w               # same, in a window, to watch it
tools/oracle/run_rpcs3.sh -s 90 -- --config my.yml   # extra arguments go to RPCS3
```

- `-w` opens a window with `--no-gui` instead of running headless.
- Arguments after `--` go to RPCS3 unchanged.
- **Only one RPCS3 at a time.** RPCS3 truncates its `RPCS3.log` and `TTY.log`
  when it starts, and the script copies them out when it ends.
- `run.txt` explains why the run ended:
  - `stopped by the Ns timer` is the normal case.
  - Anything else means RPCS3 stopped by itself. Its stop reason (e.g.
    `Reason: Failed to decrypt content`) is copied in.

### Diffing against the recomp

```sh
PS3_VERBOSE=0 ./build/tokyojungle input/EBOOT.ELF 2> recomp.log   # stop it after a similar time
tools/oracle/tty_diff.py build-oracle/<run>/TTY.log recomp.log
```

- The recomp writes guest TTY raw to stderr, mixed with runtime logging, so a
  plain `diff` won't work. `tty_diff.py` walks RPCS3's lines in order and
  reports the first one the recomp never prints, with the last few that did match.
- That missing line is where the two runs diverge. Look in `RPCS3.log` around
  the same point for the firmware calls the recomp got wrong.
- Use `PS3_VERBOSE=0`. Redirecting stderr to a file otherwise turns on the full
  runtime log, which is heavy enough to change the title's timing
  (`docs/PRIOR_PORT.md`).

## Limits of the oracle

- **RPCS3 isn't hardware.** Its wiki lists Tokyo Jungle as Playable, with
  washed-out colours on Vulkan (OpenGL is fine), map text glitches, and
  occasional audio dropouts. Don't treat RPCS3's rendering as ground truth for
  those.
- **Headless runs give logs, not frames.** Use `-w` to see what's drawn.
- **Timing differs.** The recomp and RPCS3 won't hit the same frame at the same
  second, which is why `tty_diff.py` compares order and ignores timing.
- **Verified so far:** only the mechanics, i.e. that the script boots RPCS3,
  times it out and collects its logs, and that `tty_diff.py` reports matches and
  divergence correctly. A real comparison has to wait for the licence.
