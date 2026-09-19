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
MOD_VERSION = "1.1.0"
MOD_DESCRIPTION = (
    "Tree capacitor network plus a real tree feller: rain and thunder charge "
    "capacitor roots that release growth pulses into nearby trees, and breaking "
    "one log fells the whole tree. Works across Minecraft Bedrock 1.26+ builds."
)
ENTRY = "libtree_capacitor.so"


def build_manifest() -> dict:
    return {
        "schema_version": 1,
        "id": MOD_ID,
        # `name`, `author`, `version`, `description`, `icon` and
        # `minecraft_versions` are the fields LeviLaunchroid's ModManager reads
        # out of this file (parseDirectoryMod / parseMinecraftVersions). They
        # must stay top-level: the nested "info" object used by levimod.json is
        # ignored by the launcher, which would leave the mod looking like it
        # has no compatibility metadata at all.
        "name": MOD_NAME,
        "author": MOD_AUTHOR,
        "version": MOD_VERSION,
        "description": MOD_DESCRIPTION,
        "type": "preload-native",
        "entry": ENTRY,
        # Empty = compatible with every Minecraft version. The mod resolves
        # its game hooks at runtime and falls back to the storm simulator when
        # a build is unknown, so claiming a specific version would only make
        # the launcher hide it from builds it actually works on.
        "minecraft_versions": [],
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