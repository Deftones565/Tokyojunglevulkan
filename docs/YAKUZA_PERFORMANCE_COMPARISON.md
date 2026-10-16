# Yakuza recomp comparison and the next 60 FPS experiment

Inspected 2026-10-17: [Yakuza: Dead Souls Recompiled](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled),
revision `9d08ff5`, including earlier compiler commits. This is a source comparison, not a
benchmark of Yakuza on our machine. No reference runtime code was imported into the game.

## What is actually different

| Technique | Reference evidence | Tokyo Jungle implication |
|---|---|---|
| SPU region lifting | Current [spu_lifter.py](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/tools/spu_lifter.py): `lift_regions`, normally capped at 256 instructions, internal branches become gotos, entry switch retains addressable PCs. [gen_spu_images.py](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/tools/gen_spu_images.py) uses it for the shipped regeneration path. | Best new compiler experiment. Our branches inside an existing function are already gotos, but many short functions still communicate through trampolines. |
| Whole-job register dataflow | Historical [whole-job compiler](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/3a533a4/tools/spu_wj_lifter.py) groups non-call CFG edges, carries register locals between blocks, publishes conservative dirty sets at observable boundaries, and reloads after calls. | Useful design reference, **not a demonstrated shipped win**: these WB/WJ compiler files and build options are absent at current HEAD. Our earlier broad register-caching attempt slowed image 9; live-register pressure and spills must be measured. |
| Native data-oriented job kernels | Current [scene job adapter](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/yakuza/native_scene_jobs.c) and [pose adapter](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/yakuza/native_scene_pose_job.c) validate exact descriptors, snapshot inputs, calculate compact outputs and publish completion last. | The transferable idea is deriving a proven static translation of an entire hot kernel. Their addresses, job layouts and algorithms belong to Yakuza and cannot be copied into TJ. Preserve exact outputs and publication order; no runtime decoding or approximate replacement. |
| Targeted write notifications | [ppu_guest_write.h](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/runtime/ppu/ppu_guest_write.h) uses a page bitmap before routing writes to native SPURS waiters; [cellSpurs.c](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/libs/spurs/cellSpurs.c) uses exact address/generation routing. | Worth investigating if clean measurements show polling/wake latency. Must cover every writer, including DMA/vector stores; a missed wake can deadlock. This does not automatically eliminate the real computation behind a wait. |
| SIMD, image identity cache, parallel jobs | Current helpers, workload resolver and `libs/spurs/job_parallel_pool.h`. | We already have counterparts: 0056-0058, 0061-0063, 0078, 0081-0082. Repeating these changes is not a new route to 60. Their xfloat implementation has different rounding machinery; do not swap it in casually. |
| PPU whole-program optimization | Current [CMakeLists.txt](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/yakuza/CMakeLists.txt) enables MSVC LTCG for lifted PPU code. | Our actual Ninja compile commands already use Clang `-O2`; an old comment saying target-wide `-O1` is stale. ThinLTO is an experiment, not a missing Release-mode fix, and needs a full rebuild and visual validation. |

Their current [README](https://github.com/canersaka/Yakuza-Dead-Souls-Recompiled/blob/9d08ff5/README.md) reports the 30 FPS cap in lighter scenes and about 20 FPS in the heavy
outdoor opening. The repository is valuable engineering evidence, not proof that its design
will deliver 60 FPS in another game. The historical GPU residency and whole-job evaluation
commit titles do not provide numerical results by themselves.

## Concrete opportunity in our generated code

Static census of `generated/spu/j8D485835BE6E8738/spu_recomp.c`:

- 770 functions; 584 contain at most eight non-comment statement lines.
- 858 direct tail-trampoline sites, 156 direct-call drain brackets, 43 existing native gotos.
- Counts describe code shape only. They do not establish which edges run often or how much
  frame time dispatch consumes. The 23 counted loops already tested locally have goto backedges.

Independent review by `review_spu` identified why a blind merge is unsafe: local trampoline
boundaries also run `spu_task_launch_check`, deliver pending interrupts, refresh `drain_ret_pc`,
and participate in setjmp-based interrupt return. Calls restore image identity and can return
through registers other than r0. Region formation must preserve these observable boundaries,
every existing externally callable entry, and the original call/return classification.

The small region spanning `0x3734/3740/3748/3770` is a bounded static prototype candidate.
It has arithmetic and local-store operations, but its hotness is unmeasured. Tiny blocks near
`0x3C24` illustrate why size alone is insufficient: they surround an interrupt-enable window.

## Measured baseline and next gate

Clean measurement 0083 is now in `build/tokyojungle`: native flip timestamps, no screenshot
readbacks or statistical sampler during timing, pre/post gameplay validation images. Initial
headless 720p result: **43.98 FPS = 22.74 ms/frame**. A sustained 60 needs **16.67 ms**, a reduction
of about **6.07 ms/frame**, or **26.7%** of this sample's critical-path time. This is not a
windowed, crowded-scene, or whole-game guarantee.

The production repeat was **39.27 FPS**, so the measured baseline range is 39.27-43.98 FPS,
or 22.74-25.47 ms/frame. The remaining gap is **6.07-8.80 ms/frame**. Removing profiler and
screenshot interference did not remove workload and host scheduling variation. Tighten workload
and power/core-placement controls before interpreting small changes. This host is an i7-13650HX
with both performance and efficiency cores; their effect here is still unmeasured.

Ordered-readback candidate 0084 measured **42.94 FPS**, inside the baseline repeat range,
with 0 FATAL/LIFT-GAP and matching street/character appearance. It has no demonstrated gain
and remains pending, outside the product. Evidence archives: `codex-clean83-a`,
`codex-clean83-repeat` and `codex-rb84-b`.

The next experiment should attribute the clean main-thread wait path and count hot SPU
boundary pairs in a separate diagnostic run. Then choose a small hot region, retain runtime
boundary semantics, and compare register/local-store/output state plus pending-interrupt and
non-r0-return cases. Only after that: same-script visual A/B including the Pomeranian, sequential
clean A/B/B/A performance, and `check_no_emulation`. A broad region rewrite before that evidence
would repeat the unproductive register-caching experiment.

If removing SPU work just shifts the wait to rendering, the larger remaining task is narrower
producer/consumer synchronization between SPU-produced vertex data and RSX consumption. Existing
0055/0065 waits prevent real mesh corruption. They must be replaced with proven data-lifetime
dependencies, not disabled. Our previous GPU-only blit experiment already demonstrated this
bottleneck shift, so renderer wins must be judged by total frame time.
