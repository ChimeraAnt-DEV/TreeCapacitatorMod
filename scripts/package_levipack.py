#!/usr/bin/env python3
"""Packages TreeCapacitator as a LeviLaunchroid .levipack.

Usage:
    python3 scripts/package_levipack.py --library build/libtree_capacitor.so \
        --output TreeCapacitator.levipack [--icon assets/icon.png]
"""
import argparse
import json
import sys
import zipfile
from pathlib import Path

# Keep in sync with src/Main.cpp and levimod.json.
MOD_ID = "treecapacitator"
MOD_NAME = "TreeCapacitator"
MOD_AUTHOR = "ChimeraAnt-DEV"
MOD_VERSION = "1.0.0"
MOD_DESCRIPTION = (
    "Tree capacitor network: rain and thunder charge capacitor roots that "
    "release growth pulses into nearby trees."
)
ENTRY = "libtree_capacitor.so"


def build_manifest() -> dict:
    return {
        "schema_version": 1,
        "id": MOD_ID,
        "name": MOD_NAME,
        "author": MOD_AUTHOR,
        "version": MOD_VERSION,
        "description": MOD_DESCRIPTION,
        "type": "preload-native",
        "entry": ENTRY,
        "minecraft_versions": ["1.26.33.1"],
        "icon": "icon.png",
    }


def write_package(library: Path, icon: Path | None, output: Path) -> None:
    if not library.is_file():
        raise FileNotFoundError(f"Library not found: {library}")

    manifest = build_manifest()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as archive:
        archive.writestr(
            "manifest.json",
            json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        )
        archive.write(library, ENTRY)
        if icon and icon.is_file():
            archive.write(icon, "icon.png")

    # Verify the package.
    with zipfile.ZipFile(output, "r") as archive:
        names = set(archive.namelist())
        if "manifest.json" not in names or ENTRY not in names:
            raise RuntimeError(f"Package missing required entries: {sorted(names)}")
        parsed = json.loads(archive.read("manifest.json"))
        if parsed != manifest:
            raise RuntimeError("Manifest verification failed")
        if archive.getinfo(ENTRY).file_size != library.stat().st_size:
            raise RuntimeError("Library verification failed")

    print(f"Packaged {output.resolve()}")
    print(json.dumps(manifest, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--icon", type=Path, default=None)
    args = parser.parse_args()
    try:
        write_package(args.library.resolve(), args.icon, args.output.resolve())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())