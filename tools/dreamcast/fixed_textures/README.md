# Fixed PVR texture overrides

Some N64 texture formats convert poorly to PVR at runtime. SM64 DC ships
hand-tuned replacements in `psp/textures/`; this port uses the same idea on
GD-ROM under `/cd/fixed_textures/`.

## File naming

`<fnv1a_hash>.dt` where the hash is the FNV-1a of the staged TMEM bytes (same
as `dc_texture_cache.cpp`).

## `.dt` format

| Offset | Type     | Field        |
|--------|----------|--------------|
| 0      | uint32   | magic `DCFX` |
| 4      | uint32   | width        |
| 8      | uint32   | height       |
| 12     | uint32   | stride       |
| 16     | uint32   | pvr_format   |
| 20     | uint8 x2 | fmt, siz     |
| 24     | uint16[] | ARGB1555/4444 pixels (`stride * height`) |

Stage files on disc via `tools/dreamcast/make_disc.sh` (add a `fixed_textures/`
folder beside `rom.z64`).

When a matching `.dt` is present, `dc_texture_cache` skips CPU conversion and
uploads the baked pixels directly.
