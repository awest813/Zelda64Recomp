# Dreamcast Port Progress

**Goal:** Boot from GD-ROM, load Majora's Mask, and play through the game with correct graphics, audio, input, and saves on real Dreamcast hardware.

**Target:** Sega Dreamcast (SH-4 @ 200 MHz, 16 MB RAM, 8 MB VRAM) via [KallistiOS](https://github.com/KallistiOS/KallistiOS).

**Last updated:** 2026-06-08

---

## Status legend

| Symbol | Meaning |
|--------|---------|
| ✅ | **Done** — implemented in tree; expected to work |
| 🟡 | **Partial** — started or stubbed; gaps remain |
| ❌ | **Missing** — not implemented |
| ➖ | **N/A** — intentionally excluded on Dreamcast |
| ❓ | **Unknown** — code exists but not validated on hardware |

---

## Feature matrix

### Build & toolchain

| Feature | Status | Remains |
|---------|--------|---------|
| Dreamcast CMake toolchain (`cmake/Toolchains/dreamcast.cmake`) | ✅ | — |
| KOS cross-compile integration | ✅ | — |
| Post-build `1ST_READ.BIN` packaging | ✅ | — |
| Host-side N64Recomp / RSPRecomp codegen | ✅ | Requires ROM + generated `RecompiledFuncs/` (not in repo) |
| GD-ROM disc image workflow (`scramble`, `makeip`, `mkdcdisc`) | 🟡 | Documented in `BUILDING.md`; manual steps only |
| Dreamcast CI / automated builds | ❌ | No workflow in `.github/workflows/` |
| sh4zam optimized matrix math (`-DDC_HAS_SH4ZAM`) | 🟡 | Optional; off by default |

### Core runtime

| Feature | Status | Remains |
|---------|--------|---------|
| Static recompilation core (game logic) | ✅ | Same binary path as PC after codegen |
| RSP microcode — `aspMain` (audio) | ✅ | — |
| RSP microcode — `njpgdspMain` (JPEG) | ✅ | — |
| Overlay / patch registration | ✅ | — |
| Game patches (39 files in `patches/`) | ✅ | Shared with PC; DC-specific renderer hooks vary |
| ROM decompression (Yaz0) | ✅ | — |
| Mod system | ➖ | `RECOMP_MODS_ENABLED 0`; stubs in `dc_main.cpp` |
| Texture packs | ➖ | Disabled on DC |
| Quicksaving | ➖ | Globally disabled (`#if 0` in `quicksaving.cpp`) |

### Boot & distribution

| Feature | Status | Remains |
|---------|--------|---------|
| KOS entry point (`dc_main.cpp`) | ✅ | — |
| Fixed GD-ROM ROM path (`/cd/rom.z64`) | ✅ | User must place ROM on disc |
| File picker / ROM selection UI | ➖ | Auto-loads fixed path; no dialog |
| Error display to player | 🟡 | `fprintf` / stderr only; no on-screen error UI |
| Version string (`1.2.2-dc`) | ✅ | — |

### Rendering — PVR pipeline

| Feature | Status | Remains |
|---------|--------|---------|
| PVR render context (`dc_render_context.cpp`) | ✅ | — |
| F3DZEX2 GBI high-level emulator (`dc_gbi.cpp`, ~2100 LOC) | 🟡 | Unregistered opcodes silently no-op; full MM coverage unverified |
| PVR triangle submission (`dc_pvr_renderer.cpp`) | ✅ | — |
| TMEM staging + VRAM texture cache | ✅ | 3 MB VRAM budget; may need tuning under load |
| Texture formats (RGBA16/32, IA4/8/16, CI4/CI8, I4/I8) | ✅ | — |
| Color combiner `(A-B)*C+D` | ✅ | CPU texel0 sampling when needed |
| RDP blend modes → PVR lists | ✅ | OPA/XLU/CLD/ADD, AA ZB, 2-cycle fog |
| Z-buffer + `gEXVertexZTest` occlusion | ✅ | CPU depth buffer for widescreen culling |
| Vertex fog (`G_FOG`, fog shade modes) | ✅ | — |
| `G_TEXRECT` / extended texture rectangles | ✅ | — |
| `G_MODIFYVTX`, `G_CULLDL` | ✅ | — |
| S2DEX2 `gSPBgRectCopy` | ✅ | — |
| Extended viewport / scissor / align stacks | ✅ | Widescreen patch support |
| Matrix group push/pop (`gEXMatrixGroup*`) | ✅ | — |
| VI framebuffer blit fallback | ✅ | When no Gfx tasks run |
| Per-frame actor interpolation | ❌ | PC/RT64 only (`BUILDING.md`) |
| High-precision framebuffer | ➖ | Returns `0` on DC (`recomp_api.cpp`) |
| Graphics config (resolution, MSAA, API) | ➖ | `update_config()` is a no-op; fixed 640×480 |
| Widescreen / ultrawide | 🟡 | GBI commands implemented; visual correctness & perf on DC unverified |
| RT64 / ray tracing / texture packs | ➖ | Replaced by PVR path |

### Audio

| Feature | Status | Remains |
|---------|--------|---------|
| AICA streaming (`dc_audio.cpp`) | ✅ | — |
| Sample rate / volume from config | ✅ | — |
| Sound patches (`sound_patches.c`) | ✅ | Shared with PC |
| Low-health beeps toggle | ✅ | Config-backed |

### Input & controls

| Feature | Status | Remains |
|---------|--------|---------|
| Maple bus polling | ✅ | — |
| N64 button mapping (A/B/X/Y, triggers, D-pad → C-buttons) | ✅ | — |
| Analog stick → N64 stick | ✅ | Radial deadzone |
| Puru-puru rumble | ✅ | — |
| Menu input forwarding | ✅ | Edge-detected D-pad / face buttons |
| Keyboard / mouse | ➖ | Hardware absent |
| Right analog stick | ➖ | Returns zero; no dual-analog free camera |
| Gyro aim | ➖ | Returns zero deltas |
| Input rebinding / scanning | ➖ | No-op stubs in `dc_input.cpp` |
| Sensitivity UI (gyro/mouse) | ➖ | Stub getters/setters only |
| PC control binding tables | ➖ | Bypassed (`controls.cpp` `#ifndef DREAMCAST`) |
| Analog camera mode | 🟡 | Enabled in defaults; relies on D-pad C-buttons (works) but no right-stick alternative |

### UI & menus

| Feature | Status | Remains |
|---------|--------|---------|
| Choice / info prompts (BIOS font + PVR panel) | ✅ | — |
| D-pad menu navigation | ✅ | — |
| Full RmlUi launcher | ➖ | Replaced by minimal `dc_ui.cpp` |
| In-game config menu (graphics, sound, controls) | ❌ | `set_config_tab()` no-op; no tabs |
| Mod list / configure UI | ➖ | Stubbed |
| Image / drag-drop assets | ➖ | No-ops |
| Quit prompt | 🟡 | Logs and calls `platform_shutdown()`; no confirmation UI |
| On-screen notifications | 🟡 | `open_notification()` logs to stdout only |

### Storage & saves

| Feature | Status | Remains |
|---------|--------|---------|
| VMU save / load / delete (VMS packaging) | ✅ | — |
| VMU free-block query | ✅ | — |
| Config JSON on VMU (`config.cpp` + `dc_config.cpp`) | ✅ | Persists; no in-game editor to change values |
| Autosaving patch + VMU backend | ✅ | Shared autosave logic |
| GD-ROM file read | ✅ | — |
| Multi-file dialogs | ➖ | Returns failure |

### Gameplay (shared recompiled code)

| Feature | Status | Remains |
|---------|--------|---------|
| Full MM scene / warp table | ✅ | `scene_table.cpp` |
| Debug warps / set time | ✅ | `debug.cpp` (if debug mode enabled) |
| Targeting mode / camera invert APIs | ✅ | Config-backed |
| Game API bridge (`recomp_api.cpp`, actor/data APIs) | ✅ | — |
| Playthrough from title → credits | ❓ | **Not validated** — primary “fully playable” gate |

### Performance & hardware

| Feature | Status | Remains |
|---------|--------|---------|
| 16 MB RAM footprint | ❓ | No profiling data; likely needs optimization |
| 200 MHz SH-4 frame rate | ❓ | Target unknown; may require LOD / culling / GBI tuning |
| VRAM texture cache pressure | ❓ | 3 MB budget; eviction behavior under heavy scenes untested |
| Real-hardware test pass | ❌ | No test matrix or hardware CI |

---

## Summary by area

```
Build & toolchain     ████████░░  80%   (CI + disc automation missing)
Core runtime          █████████░  90%   (codegen external; quicksave N/A)
Boot & distribution   ███████░░░  70%   (works; weak error UX)
Rendering (PVR)       ████████░░  80%   (broad GBI; interpolation & HW verify)
Audio                 ██████████ 100%
Input & controls      ███████░░░  70%   (playable mapping; no rebind UI)
UI & menus            ████░░░░░░  40%   (prompts only; no config launcher)
Storage & saves       █████████░  90%   (VMU done; config edit UX missing)
Gameplay              ░░░░░░░░░░   ?%   (needs end-to-end hardware validation)
Performance           ░░░░░░░░░░   ?%   (biggest unknown for “fully playable”)
```

---

## Critical path to “fully playable”

Ordered by dependency. Items marked **blocker** must be resolved before the port can be called playable on hardware.

| # | Item | Priority | Notes |
|---|------|----------|-------|
| 1 | **End-to-end hardware boot test** | Blocker | Build disc, boot on DC, reach title screen |
| 2 | **In-game rendering correctness** | Blocker | Fix GBI gaps found during play (silent no-ops today) |
| 3 | **Performance / RAM** | Blocker | Profile on SH-4; optimize hot paths, texture budget, frame pacing |
| 4 | **Playthrough validation** | Blocker | Title → Clock Town → dungeons → major scenes without crash/hang |
| 5 | **VMU save/load in real play** | High | Verify autosave + manual save across power cycle |
| 6 | **Audio sync & dropouts** | High | Stress AICA buffer under load |
| 7 | **On-screen error reporting** | Medium | Replace stderr-only failures (missing ROM, VMU full) |
| 8 | **Minimal config UI** | Medium | Volume, autosave, targeting — values persist but are not editable in-game |
| 9 | **Widescreen visual QA** | Low | GBI support exists; verify patches on 4:3 DC output |
| 10 | **Disc build automation** | Low | Script `scramble` / `makeip` / `mkdcdisc` in CI or Makefile |
| 11 | **DC CI workflow** | Low | Cross-compile on push (no hardware required) |

---

## Intentionally out of scope (Dreamcast)

These PC features are disabled by design (`include/dreamcast_platform.h`) and are **not** required for baseline playability:

- RT64 renderer, high FPS, MSAA, ray tracing
- RmlUi full launcher and styled config menus
- SDL2 input, keyboard/mouse, gyro, dual analog
- Mod system and texture packs
- Native file dialogs (ROM is on-disc)
- Quicksaving (disabled on all platforms)

---

## Key source files

| Area | Path |
|------|------|
| Platform flags | `include/dreamcast_platform.h` |
| Entry / stubs | `src/main/dreamcast/dc_main.cpp` |
| PVR + GBI | `src/main/dreamcast/dc_{render_context,gbi,pvr_renderer,texture_cache,combiner,rdp_blend,zbuffer,math}.cpp` |
| Audio | `src/main/dreamcast/dc_audio.cpp` |
| Platform / VMU / GD-ROM | `src/main/dreamcast/dc_support.cpp` |
| Input | `src/game/dreamcast/dc_input.cpp` |
| Config | `src/game/dreamcast/dc_config.cpp` |
| UI | `src/ui/dreamcast/dc_ui.cpp` |
| Build docs | `BUILDING.md` § “Building for Dreamcast (Experimental)” |
| Toolchain | `cmake/Toolchains/dreamcast.cmake` |

---

## How to update this document

1. After hardware tests, change ❓ → ✅ or 🟡 and note findings in the **Remains** column.
2. When a GBI or renderer gap is found, add a row under **Rendering** with the opcode/scene.
3. Bump **Last updated** and adjust the summary bars when a whole area moves forward.
