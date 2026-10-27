#!/usr/bin/env python3
"""Colour-budget gate — DESIGN.md 8.3, check #1.

Fails when a raw 32-bit colour literal (0xAARRGGBB) appears anywhere in
Source/ outside the whitelisted definition points. Without this gate the
near-black count crept from 6 tokens to 80 distinct values in a few months;
with it, drift is caught the moment someone types a hex instead of a token.

Run:  python Tools/check_colours.py        (exit 0 = pass, 1 = drift)

Whitelist policy:
  * Util/Theme.h              — the 18 tokens themselves.
  * Model/LayerModel.h        — layer/group identity palettes (serialised
                                into .atomic files, owned by the model).
  * Model/HardwareRack.*      — instrument-slot identity palette + default.
  * Model/ProjectIO.cpp       — one load fallback for pre-v3 files.
  * PianoRollComponent.cpp    — the pencil cursor's skeuomorphic icon colours
                                (wood grain, brass ferrule, eraser). These
                                depict a physical object, not UI state.
  * 0xffffffff                — a bit mask, not a colour.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "Source"

# file (posix rel path) -> set of allowed literals; empty set = all allowed
WHITELIST = {
    "Util/Theme.h": set(),
    "Model/LayerModel.h": set(),
    "Model/HardwareRack.h": set(),
    "Model/HardwareRack.cpp": set(),
    "Model/ProjectIO.cpp": {0xFF7C96F5},
    "UI/PianoRollComponent.cpp": {0xFFF2D9A0, 0xFFE8B84B, 0xFFB6BCC8,
                                  0xFFEF8FA3, 0xFFF4F0F6},
}

LITERAL = re.compile(r"\b0x[0-9A-Fa-f]{8}\b")
MASKS = {0xFFFFFFFF}          # bit masks, never colours

def main() -> int:
    violations = []
    for f in sorted(ROOT.rglob("*")):
        if f.suffix not in (".cpp", ".h"):
            continue
        rel = f.relative_to(ROOT.parent).as_posix().replace("Source/", "", 1)
        rel = f.relative_to(ROOT).as_posix() if False else f.relative_to(ROOT).as_posix()
        allowed = WHITELIST.get(rel)
        if allowed is not None and not allowed:
            continue                      # definition point: everything allowed
        for i, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
            for m in LITERAL.finditer(line):
                v = int(m.group(0), 16)
                if v in MASKS:
                    continue
                if allowed is not None and v in allowed:
                    continue
                violations.append(f"{rel}:{i}: 0x{v:08x}  {line.strip()[:80]}")
    if violations:
        print(f"COLOUR DRIFT — {len(violations)} literal(s) outside the palette:\n")
        print("\n".join(violations))
        print("\nAdd it to Util/Theme.h as a named token, or extend the whitelist")
        print("in Tools/check_colours.py with a written justification.")
        return 1
    print("colour budget OK: no literals outside the whitelisted definition points")
    return 0

if __name__ == "__main__":
    sys.exit(main())
