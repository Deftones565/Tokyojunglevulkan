# SPU per-function bisect (SSE4.1 pshufb shufb)

Finds which lifted SPU function misbehaves when built with the inlined pshufb `spu_shufb`
(`-msse4.1`). Written for image 9 (`j8D485835BE6E8738`), works for any image.

- `fsplit.py <src spu_recomp.c> <dst> <lo> <hi>`: writes a copy of an image's lifted TU in which
  only the shufb-using functions with index in [lo, hi) keep the pshufb version; every other
  function is wrapped in `#define spu_shufb spu_shufb_ref` (the byte-loop reference). Prints the
  kept function names. Use `<dst>=/dev/null` to just list a range.
- `run.sh <name>`: one 240 s run of `$BISECT_BUILD` (default build-mel) with the 32-press script
  to the tutorial, frame dumps to `build-runs/<name>/frames`, posting RUN start/done on the board
  as `$BISECT_AGENT`. Dump window: `BISECT_EVERY` (presents per dump, default 30),
  `BISECT_SKIP` (dumps to drop, default 340), `BISECT_MAX` (dumps to keep, default 80). Dumps count
  presents, not flips, so a run under load reaches a given dump later: use a wide window
  (e.g. EVERY=10 SKIP=800 MAX=450).

Setup: a private build dir configured with `-DSPU_DIR=<private copy of generated/spu>`,
`-DPORT_SPU_EXTRA_FLAGS="-msse4.1;-ffp-contract=off" -DPORT_SPU_EXTRA_FLAGS_IMAGES=<image>`; write
fsplit's output over `<private SPU_DIR>/<image>/spu_recomp.c`, `ninja -C <dir> -j2` (~10 s).

Verdict: broken = the Pomeranian close-up at the start of the tutorial shows shards. It is
intermittent (1-2 shard frames out of ~5-20 close-up frames), so CLEAN needs zero shard frames
over >= 15 close-up frames.
