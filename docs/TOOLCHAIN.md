# Toolchain

What's vendored, whether it builds on this machine, and what we can render with.
Checked 2026-10-14 by MELCHIOR-1.

## Vendored

| Path | Source | Pinned at |
|---|---|---|
| `third_party/ps3recomp` | github.com/sp00nznet/ps3recomp (MIT) | `a2c2010` (v0.14.0) |
| `third_party/tokyojungle-ref` | github.com/sp00nznet/tokyojungle | `5b7cc00` (master) |

Both are git submodules. Clone with `git submodule update --init`.
`tokyojungle-ref` is for reference only. We don't build it as-is (see below).

## Host

Arch Linux, kernel 7.2.6

| Tool | Version |
|---|---|
| clang | 22.1.8 |
| gcc | 16.2.1 |
| cmake | 4.4.3 |
| ninja | 1.13.2 |
| python | 3.14.7 |
| Vulkan loader | 1.4.357 |
| glslang | 16.4.0 |
| SDL2 / SDL3 / GLFW | 2.32.72 / 3.4.16 / 3.5.1 |

GPUs: NVIDIA RTX 5060 Laptop (Vulkan 1.4.351, picked by default) and Intel RPL-S iGPU.

## ps3recomp runtime builds on Linux

Both configurations build cleanly with clang and pass their tests. Builds are out-of-tree,
so the submodule stays clean (`build*/` is gitignored):

```sh
# default: null (headless) renderer
cmake -S third_party/ps3recomp -B build-ps3recomp -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS3RECOMP_BUILD_TESTS=ON
cmake --build build-ps3recomp

# Vulkan renderer
cmake -S third_party/ps3recomp -B build-ps3recomp-vk -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS3RECOMP_BUILD_TESTS=ON \
  -DPS3RECOMP_RSX_VULKAN=ON
cmake --build build-ps3recomp-vk
```

| | default | Vulkan |
|---|---|---|
| Build | OK, 184 steps, about 6 s | OK, 186 steps, about 7 s |
| Compiler warnings | 34 lines (none fatal) | 34 lines (none fatal) |
| ctest | 3/3 pass | 3/3 pass |
| Output | `libps3recomp_runtime.a`, `ps3recomp_host`, `ps3recomp_boot_smoke` | same |

Configure detected:
- SDL2, so the input and audio backends are on.
- Vulkan headers and glslang. Guest shader programs are available at runtime with
  `PS3RECOMP_VK_GUEST_PROGRAMS=1`.
- No spirv-cross. That only affects Metal, so it doesn't matter here.

`build-ps3recomp-vk/ps3recomp_host` (no game; cellGcm → RSX → backend) ran on the RTX 5060 and
passed its clear-through-FIFO and presented-pixel checks.

## Graphics backends

`third_party/ps3recomp/libs/video/`:

| Backend | File | Platform | Status per upstream README |
|---|---|---|---|
| D3D12 | `rsx_d3d12_backend.c` | Windows | Default on Windows. The only backend that has booted a game in-tree. |
| Vulkan | `rsx_vulkan_backend.c` | Linux (opt-in) | Passes every `ps3recomp_host` scene, including RTT, MRT, depth textures, cube maps and stencil. Downstream, Twisted Metal's menus run on it. |
| Metal | `rsx_metal_backend.m` | macOS | Parity with Vulkan on the host scenes |
| null | `rsx_null_backend.c` | any | Headless CPU triangle filler. Linux default; used by CI. |

There's no OpenGL backend. **On this box, the backend to use is Vulkan.**

## Caveats

- **Upstream says no game has booted on Linux from its own tree.** The per-game PPU boot
  scaffold (`ppu_loader.cpp`, `boot_main.cpp`, HLE dispatch) compiles on POSIX but hasn't
  been taken to gameplay there. Downstream ports have done it with patches that aren't
  upstream yet. Expect to carry some fixes of our own.
- **`tokyojungle-ref` is Windows/D3D12-only as written.** Its `CMakeLists.txt` links `d3d12`
  under `if(WIN32)` and has MSVC-specific flags. It looks for ps3recomp at
  `../ps3recomp` (override with `-DPS3RECOMP_DIR`).
- **`tokyojungle-ref` doesn't ship the lifted game code.** `src/` (308 KB) holds only glue,
  stubs, `gen/ppu_hle_nids.cpp` and `gen/spu_workloads.c`. The recompiled PPU code has to be
  regenerated from our own decrypted EBOOT.
- **The reference port's renderer is unfinished.** Per its README, it opens a window, clears and
  presents, but draws no geometry. The open question there is why the draw calls never reach
  the backend.

## Python

`.venv/` (gitignored) holds ps3recomp's `requirements.txt` deps:
pycryptodome 4.0.0, capstone 5.0.9, construct 2.10.70, tabulate 0.10.0, tqdm 4.70.1.

```sh
.venv/bin/python third_party/ps3recomp/tools/<tool>.py ...
```
