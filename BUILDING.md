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

After building, create a bootable GD-ROM disc image:

```bash
cd build-dreamcast
# Scramble the binary (required for Dreamcast boot)
scramble 1ST_READ.BIN 1ST_READ.BIN
# Create IP.BIN (bootstrap)
makeip /path/to/ip.txt IP.BIN
# Create disc image
mkdcdisc -e 1ST_READ.BIN -o zelda64recomp.cdi -n "ZELDA64 RECOMP"
```

Place the game ROM on the disc as `/cd/rom.z64`.

### Important Notes

- **This port is highly experimental.** The Dreamcast has only 16 MB of RAM and a 200 MHz SH-4 CPU — performance is expected to be challenging.
- The RT64 renderer is replaced with a PVR (PowerVR2) renderer. A software F3DZEX2 display list interpreter rasterizes Gfx/RDP commands into RDRAM; the presentation path then blits that framebuffer to the screen. This is an early implementation: textured geometry, combiners, depth, and S2DEX2 are not yet supported.
- The RmlUi menu system is replaced with a minimal BIOS-font-based menu.
- The mod system is disabled on Dreamcast.
- Save data is stored on VMU (Visual Memory Unit).
- SDL2 is not used; input comes directly from the Dreamcast maple bus.
