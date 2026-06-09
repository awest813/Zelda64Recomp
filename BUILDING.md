# Building Guide

This guide will help you build the project on your local machine. The process will require you to provide a decompressed ROM of the US version of the game.

These steps cover: decompressing the ROM, running the recompiler and finally building the project.

## 1. Clone the Zelda64Recomp Repository
This project makes use of submodules so you will need to clone the repository with the `--recurse-submodules` flag.

```bash
git clone --recurse-submodules
# if you forgot to clone with --recurse-submodules
# cd /path/to/cloned/repo && git submodule update --init --recursive
```

## 2. Install Dependencies

### Linux
For Linux the instructions for Ubuntu are provided, but you can find the equivalent packages for your preferred distro.

```bash
# For Ubuntu, simply run:
sudo apt-get install cmake ninja-build libsdl2-dev libgtk-3-dev lld llvm clang
```

### Windows
You will need to install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/).
In the setup process you'll need to select the following options and tools for installation:
- Desktop development with C++
- C++ Clang Compiler for Windows
- C++ CMake tools for Windows

The other tool necessary will be `make` which can be installe via [Chocolatey](https://chocolatey.org/):
```bash
choco install make
```

## 3. Decompressing the target ROM
You will need to decompress the NTSC-U N64 Majora's Mask ROM (sha1: d6133ace5afaa0882cf214cf88daba39e266c078) before running the recompiler.

There are a few tools that can do it:
* This python script from the Majora's Mask decompilation project: https://github.com/zeldaret/mm/blob/main/tools/decompress_baserom.py
* https://github.com/z64tools/z64decompress

Regardless of which method you use, copy the decompressed ROM to the root of the Zelda64Recomp repository with this filename:
- `mm.us.rev1.rom_uncompressed.z64`

## 4. Generating the C code

Now that you have the required files, you must build [N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp) and run it to generate the C code to be compiled. The building instructions can be found [here](https://github.com/Mr-Wiseguy/N64Recomp?tab=readme-ov-file#building). That will build the executables: `N64Recomp` and `RSPRecomp` which you should copy to the root of the Zelda64Recomp repository.

After that, go back to the repository root, and run the following commands:
```bash
./N64Recomp us.rev1.toml
./RSPRecomp aspMain.us.rev1.toml
./RSPRecomp njpgdspMain.us.rev1.toml
```

## 5. Building the Project

Finally, you can build the project! :rocket:

On Windows, you can open the repository folder with Visual Studio, and you'll be able to `[build / run / debug]` the project from there.

If you prefer the command line or you're on a Unix platform you can build the project using CMake:

```bash
cmake -S . -B build-cmake -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -G Ninja -DCMAKE_BUILD_TYPE=Release # or Debug if you want to debug
cmake --build build-cmake --target Zelda64Recompiled -j$(nproc) --config Release # or Debug
```

## 6. Success

Voilà! You should now have a `Zelda64Recompiled` executable in the build directory! If you used Visual Studio this will be `out/build/x64-[Configuration]` and if you used the provided CMake commands then this will be `build-cmake`. You will need to run the executable out of the root folder of this project or copy the assets folder to the build folder to run it.

> [!IMPORTANT]
> In the game itself, you should be using a standard ROM, not the decompressed one.

## Building for Dreamcast (Experimental)

The Dreamcast port targets the Sega Dreamcast hardware using [KallistiOS (KOS)](https://github.com/KallistiOS/KallistiOS) as the OS/SDK.

### Prerequisites

1. **KallistiOS toolchain**: Follow the [KOS build guide](https://github.com/KallistiOS/KallistiOS/wiki/Getting-Started) to install the `sh-elf` cross-compiler and build KOS.
2. Set environment variables:
   ```bash
   export KOS_BASE=/path/to/kos
   export KOS_CC_BASE=/path/to/sh-elf  # e.g., /opt/toolchains/dc/sh-elf
   ```
3. Complete steps 1–4 above (clone, dependencies, ROM, C code generation) on a PC first — the recompiler runs on the host, not on Dreamcast.

### Building

```bash
cmake -S . -B build-dreamcast \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/dreamcast.cmake \
    -DKOS_BASE=$KOS_BASE \
    -DKOS_CC_BASE=$KOS_CC_BASE \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-dreamcast --target Zelda64Recompiled -j$(nproc)
```

### Creating a Disc Image

Use the bundled script, which drives [mkdcdisc](https://gitlab.com/simulant/mkdcdisc). mkdcdisc takes the *unscrambled* ELF and generates IP.BIN and the scrambled `1ST_READ.BIN` itself — do not pre-scramble anything:

```bash
tools/dreamcast/make_disc.sh \
    -e build-dreamcast/Zelda64Recompiled \
    -r /path/to/mm.us.rev1.z64 \
    -o zelda64recomp.cdi
```

The script stages the ROM at the disc root as `rom.z64` (the path the game loads at boot) and optionally adds an asset directory with `-a`. Alternatively, configure with `-DDC_ROM_FOR_DISC=/path/to/rom.z64` and run `cmake --build build-dreamcast --target dc_disc`.

The resulting `.cdi` boots in Flycast/lxdream or can be burned for a real console. At boot the game auto-loads `/cd/rom.z64` (no launcher); a missing or wrong ROM shows an on-screen error. Saves and config are mirrored to a VMU in slot A1 as a single `ZELDA64.SAV` file.

### Important Notes

- **This port is highly experimental.** The Dreamcast has only 16 MB of RAM and a 200 MHz SH-4 CPU — performance is expected to be challenging.
- Optional: link against [sh4zam](https://github.com/gyrovorbis/sh4zam) from kos-ports and pass `-DDC_HAS_SH4ZAM` to enable SH-4 optimized matrix math.
- The RT64 renderer is replaced with a PVR (PowerVR2) renderer using the SM64 DC port architecture: a F3DZEX2 high-level emulator submits triangles and fill rectangles directly to the PVR tile accelerator. N64 textures are staged in a 4 KB TMEM buffer and imported via a VRAM texture cache (RGBA16/32, IA4/8/16, CI4/CI8, I4/I8). `G_TEXRECT`, extended `gEXTextureRectangle`, full `(A-B)*C+D` color combiners with CPU texel0 sampling when the combine mode references `TEXEL0`, `G_MODIFYVTX`, `G_CULLDL`, S2DEX2 `gSPBgRectCopy`, polygon-context batching, and sh4zam-accelerated matrix math (when available) are supported. Extended GBI viewport/scissor stacks and align commands used by widescreen patches are implemented. Matrix-group push/pop (`gEXMatrixGroup*` / `gEXPopMatrixGroup`) is implemented for stack correctness; per-frame interpolation tagging remains a PC/RT64 feature. Vertex fog (`G_FOG`, `G_MW_FOG`, `G_SETFOGCOLOR`, `G_RM_FOG_SHADE_A`) is emulated by computing per-vertex screen-Z fog alpha and blending toward the fog color after the color combiner. RDP render modes (`G_RM_OPA_SURF`, `G_RM_XLU_SURF`, `G_RM_CLD_SURF`, `G_RM_ADD`, `G_RM_AA_ZB_*`, 2-cycle fog+surf pairs, etc.) are decoded from `other_mode_l`/`other_mode_h` into PVR list routing, blend factors, depth compare/write (`ZMODE_OPA`/`XLU`/`DEC`/`INTER`), and `G_AC_THRESHOLD` punch-through alpha test (using `blend_color` alpha as the RDP threshold). Z-buffering (`G_ZBUFFER`, `G_SETZIMG`) drives PVR depth compare/write and a CPU-side depth buffer used by `gEXVertexZTest` / `gEXEndVertexZTest` for widescreen-safe occlusion (e.g. light glow culling). The minimal menu overlay composites a translucent PVR background panel before `pvr_scene_finish()`, with BIOS-font text drawn to the framebuffer afterward. The VI framebuffer blit path remains as a fallback when no Gfx tasks run.
- The RmlUi menu system is replaced with a minimal BIOS-font-based menu.
- The mod system is disabled on Dreamcast.
- Save data is stored on VMU (Visual Memory Unit).
- SDL2 is not used; input comes directly from the Dreamcast maple bus.
