#!/usr/bin/env python3
"""Compare Zelda64 Recomp GBI dispatch against SM64 DC gfx_retro_dc.c.

Run during hardware playtests when dc_gbi logs unimplemented opcodes:

  python3 tools/dreamcast/compare_gbi_dispatch.py \\
      --zelda src/main/dreamcast/dc_gbi.cpp \\
      --sm64 /path/to/sm64-dc/src/pc/gfx/gfx_retro_dc.c

If --sm64 is omitted, the script clones jnmartin84/sm64-dc shallowly to a temp dir.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ZELDA_DISPATCH_RE = re.compile(r"gbi_dispatch\[(G_[A-Z0-9_]+)\]\s*=\s*(\w+)")
SM64_CASE_RE = re.compile(r"^\s*case\s+(G_[A-Z0-9_]+)\s*:")


def parse_zelda(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    dispatch: dict[str, str] = {}
    for match in ZELDA_DISPATCH_RE.finditer(text):
        dispatch[match.group(1)] = match.group(2)
    return dispatch


def parse_sm64(path: Path) -> set[str]:
    opcodes: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SM64_CASE_RE.match(line)
        if match:
            opcodes.add(match.group(1))
    return opcodes


def ensure_sm64(path: Path | None) -> Path:
    if path is not None:
        if not path.is_file():
            raise SystemExit(f"SM64 file not found: {path}")
        return path

    tmp = Path(tempfile.mkdtemp(prefix="sm64-dc-"))
    repo = tmp / "sm64-dc"
    subprocess.run(
        [
            "git",
            "clone",
            "--depth",
            "1",
            "https://github.com/jnmartin84/sm64-dc.git",
            str(repo),
        ],
        check=True,
    )
    candidate = repo / "src/pc/gfx/gfx_retro_dc.c"
    if not candidate.is_file():
        raise SystemExit(f"Expected SM64 gfx file missing: {candidate}")
    return candidate


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--zelda",
        type=Path,
        default=Path("src/main/dreamcast/dc_gbi.cpp"),
        help="Zelda dc_gbi.cpp path",
    )
    parser.add_argument(
        "--sm64",
        type=Path,
        default=None,
        help="SM64 gfx_retro_dc.c path (optional; clones repo if omitted)",
    )
    args = parser.parse_args()

    zelda = parse_zelda(args.zelda)
    sm64_path = ensure_sm64(args.sm64)
    sm64 = parse_sm64(sm64_path)

    zelda_ops = set(zelda.keys())
    implemented = zelda_ops & sm64
    zelda_only = sorted(zelda_ops - sm64)
    sm64_only = sorted(sm64 - zelda_ops)
    unimplemented = sorted(name for name, handler in zelda.items() if handler == "dl_unimplemented")

    print(f"Zelda dispatch entries: {len(zelda_ops)}")
    print(f"SM64 case opcodes:      {len(sm64)}")
    print(f"Shared opcode names:    {len(implemented)}")
    print()

    if unimplemented:
        print("Zelda handlers still dl_unimplemented:")
        for name in unimplemented:
            print(f"  {name}")
        print()

    if sm64_only:
        print("SM64 gfx_retro_dc.c opcodes missing from Zelda init_dispatch:")
        for name in sm64_only:
            print(f"  {name}")
        print()

    if zelda_only:
        print("Zelda-only dispatch entries (not seen as SM64 case labels):")
        for name in zelda_only:
            print(f"  {name} -> {zelda[name]}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
