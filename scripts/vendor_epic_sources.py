#!/usr/bin/env python3
"""Vendor the Persona blend-space triangulation helpers from a UE4.27 install.

The plugin's headless blend-space retriangulation (`digbp anim edit blendspace
...`) uses four source files from Engine/Source/Editor/Persona/Private. They
are module-private in the Persona module (not linkable from a plugin), so they
must be compiled into BlueprintAnalyzer directly. Epic's source may not be
redistributed outside the UE EULA, so this repository does not include them;
every UE licensee already has them on disk, and this script copies them from
your engine install and applies the two mechanical changes the plugin needs:

  * wraps each file in `namespace DigBSGrid { ... }` (unity-build safety), and
  * adds an `#include "Animation/BlendSpaceBase.h"` to the 1D header so the
    elaborated references to ::FBlendParameter / ::FEditorElement resolve to
    the engine structs instead of silently declaring new DigBSGrid:: types.

Usage:
    python scripts/vendor_epic_sources.py <path-to-UE4.27-root>

where <path-to-UE4.27-root> is the directory containing Engine/ (e.g.
C:/Program Files/Epic Games/UE_4.27 or a source-build root). Written for the
stock 4.27 layout of these files; if Epic's lines have shifted (a heavily
patched engine), the anchor check below fails loudly rather than emitting a
file that compiles wrong.
"""

import sys
from pathlib import Path

CRLF = b"\r\n"

VENDOR_COMMENT = [
    "// Vendored from Engine/Source/Editor/Persona/Private (4.27) — module-private",
    "// there, so not linkable; namespace-wrapped for unity-build safety. Do not edit; re-vendor on engine update.",
]
NS_OPEN = VENDOR_COMMENT + ["namespace DigBSGrid {", ""]
NS_OPEN_1D_HEADER = [
    "// Added on vendoring: bring the real ::FBlendParameter / ::FEditorElement into",
    "// scope BEFORE the namespace opens. Without this, the elaborated-type",
    "// references below would silently declare new incomplete DigBSGrid:: types",
    "// instead of referring to the engine structs.",
    '#include "Animation/BlendSpaceBase.h"',
    "",
] + VENDOR_COMMENT + ["namespace DigBSGrid {"]
NS_CLOSE = ["", "} // namespace DigBSGrid"]

# (file, insert-after 1-based line, (anchor 1-based line, anchor prefix), open block)
FILES = [
    ("AnimationBlendSpaceHelpers.h", 10, (9, "struct FTriangle;"), NS_OPEN),
    ("AnimationBlendSpaceHelpers.cpp", 10, (9, "#define LOCTEXT_NAMESPACE"), NS_OPEN),
    ("AnimationBlendSpace1DHelpers.h", 5, (5, '#include "CoreMinimal.h"'), NS_OPEN_1D_HEADER),
    ("AnimationBlendSpace1DHelpers.cpp", 7, (6, "#define LOCTEXT_NAMESPACE"), NS_OPEN),
]


def vendor(src: Path, dest: Path, insert_after: int, anchor, open_block) -> None:
    data = src.read_bytes()
    if CRLF not in data:
        raise SystemExit(f"{src}: expected CRLF line endings; refusing to guess")
    lines = data.split(CRLF)

    anchor_line, anchor_prefix = anchor
    got = lines[anchor_line - 1].decode("utf-8", "replace").strip()
    if not got.startswith(anchor_prefix):
        raise SystemExit(
            f"{src}: line {anchor_line} is {got!r}, expected it to start with "
            f"{anchor_prefix!r} — engine layout differs from stock 4.27; vendor by hand"
        )

    out = (
        lines[:insert_after]
        + [l.encode("utf-8") for l in open_block]
        + lines[insert_after:-1]  # drop the trailing empty element from the final CRLF
        + [l.encode("utf-8") for l in NS_CLOSE]
        + [b""]
    )
    dest.write_bytes(CRLF.join(out))
    print(f"vendored {dest}")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    persona = Path(sys.argv[1]) / "Engine/Source/Editor/Persona/Private"
    if not persona.is_dir():
        raise SystemExit(f"not found: {persona} — pass the directory containing Engine/")
    dest_dir = Path(__file__).resolve().parent.parent / (
        "BlueprintAnalyzer/Source/BlueprintAnalyzer/Private"
    )
    for name, insert_after, anchor, open_block in FILES:
        vendor(persona / name, dest_dir / name, insert_after, anchor, open_block)


if __name__ == "__main__":
    main()
