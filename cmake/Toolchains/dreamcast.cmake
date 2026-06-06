# CMake toolchain file for Sega Dreamcast (KallistiOS / SH-4)
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/dreamcast.cmake \
#         -DKO_BASE=/opt/toolchains/dc \
#         -DKOS_BASE=/opt/toolchains/dc/kos \
#         -S . -B build-dreamcast -G Ninja
#
# Environment variables (alternative to -D flags):
#   KOS_BASE  - path to the KallistiOS source tree
#   KOS_CC_BASE - path to the SH-4 cross-compiler prefix (e.g. /opt/toolchains/dc/sh-elf)
#
# The KOS SDK must be built and installed before using this toolchain.

cmake_minimum_required(VERSION 3.20)

# ── Platform identity ────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR sh4)
set(DREAMCAST TRUE)

# ── Locate KOS ───────────────────────────────────────────────────────
if(NOT DEFINED KOS_BASE)
    if(DEFINED ENV{KOS_BASE})
        set(KOS_BASE $ENV{KOS_BASE})
    else()
        message(FATAL_ERROR "KOS_BASE must be set (path to KallistiOS source tree)")
    endif()
endif()

if(NOT DEFINED KOS_CC_BASE)
    if(DEFINED ENV{KOS_CC_BASE})
        set(KOS_CC_BASE $ENV{KOS_CC_BASE})
    else()
        # Default: assume the cross-compiler lives alongside KOS
        set(KOS_CC_BASE "${KOS_BASE}/../sh-elf")
    endif()
endif()

set(KOS_ARCH "dreamcast")
set(KOS_SUBARCH "pristine")

# ── Cross-compiler ───────────────────────────────────────────────────
set(CROSS_PREFIX "${KOS_CC_BASE}/bin/sh-elf-")

set(CMAKE_C_COMPILER   "${CROSS_PREFIX}gcc"   CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${CROSS_PREFIX}g++"   CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${CROSS_PREFIX}gcc"   CACHE FILEPATH "" FORCE)
set(CMAKE_AR           "${CROSS_PREFIX}ar"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${CROSS_PREFIX}ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER       "${CROSS_PREFIX}ld"     CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY      "${CROSS_PREFIX}objcopy" CACHE FILEPATH "" FORCE)
set(CMAKE_SIZE         "${CROSS_PREFIX}size"    CACHE FILEPATH "" FORCE)

# ── SH-4 CPU flags ──────────────────────────────────────────────────
# -ml: little-endian SH-4
# -m4-single-only: SH-4 FPU in single-precision-only mode (Dreamcast default)
# -ffunction-sections / -fdata-sections: enable --gc-sections at link time
set(DC_CPU_FLAGS "-ml -m4-single-only -ffunction-sections -fdata-sections")

# ── KOS include/library paths ───────────────────────────────────────
set(KOS_INC_DIRS
    "${KOS_BASE}/include"
    "${KOS_BASE}/kernel/arch/${KOS_ARCH}/include"
    "${KOS_BASE}/addons/include"
    "${KOS_CC_BASE}/sh-elf/include"
)

set(KOS_LIB_DIRS
    "${KOS_BASE}/lib/${KOS_ARCH}"
    "${KOS_BASE}/addons/lib/${KOS_ARCH}"
)

# ── KOS defines ──────────────────────────────────────────────────────
set(KOS_DEFINES "-D_arch_dreamcast -D_arch_sub_${KOS_SUBARCH} -DDREAMCAST")

# ── Compiler flags ───────────────────────────────────────────────────
set(KOS_INC_FLAGS "")
foreach(dir ${KOS_INC_DIRS})
    string(APPEND KOS_INC_FLAGS " -isystem ${dir}")
endforeach()

set(CMAKE_C_FLAGS_INIT   "${DC_CPU_FLAGS} ${KOS_DEFINES} ${KOS_INC_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${DC_CPU_FLAGS} ${KOS_DEFINES} ${KOS_INC_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${DC_CPU_FLAGS} ${KOS_DEFINES} ${KOS_INC_FLAGS}")

# C++ standard: KOS GCC 13+ supports C++20 but some library features may
# be missing; the main build will downgrade as needed via #ifdef DREAMCAST.

# ── Linker flags ─────────────────────────────────────────────────────
set(KOS_LIB_FLAGS "")
foreach(dir ${KOS_LIB_DIRS})
    string(APPEND KOS_LIB_FLAGS " -L${dir}")
endforeach()

set(KOS_LD_SCRIPT "${KOS_BASE}/utils/ldscripts/shlelf.xc")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-ml -m4-single-only -Wl,-Ttext=0x8c010000 -Wl,--gc-sections ${KOS_LIB_FLAGS} -T${KOS_LD_SCRIPT} -lkallisti -lc -lgcc"
)

# ── Search paths ─────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH "${KOS_BASE}" "${KOS_CC_BASE}/sh-elf")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# ── Disable try_compile for cross-compilation ────────────────────────
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
