#!/usr/bin/env python3
"""Validates a TreeCapacitator .levipack matches the expected layout."""
import json
import sys
import zipfile

EXPECTED_LIBRARY = "libtree_capacitor.so"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify_levipack.py <file.levipack>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    with zipfile.ZipFile(path) as archive:
        names = set(archive.namelist())
        if "manifest.json" not in names:
            print("missing manifest.json", file=sys.stderr)
            return 1
        if EXPECTED_LIBRARY not in names:
            print(f"missing {EXPECTED_LIBRARY}", file=sys.stderr)
            return 1
        manifest = json.loads(archive.read("manifest.json"))
        if manifest.get("type") != "preload-native":
            print("manifest type is not preload-native", file=sys.stderr)
            return 1
        if manifest.get("entry") != EXPECTED_LIBRARY:
            print("manifest entry mismatch", file=sys.stderr)
            return 1
    print(f"OK: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())