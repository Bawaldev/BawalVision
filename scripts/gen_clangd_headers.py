#!/usr/bin/env python3
"""Append header entries to a CMake compile_commands.json for clangd.

clangd only finds exact flags for files listed in the compilation database.
Headers are not compiled, so it falls back to a heuristic command (often a
dependency under build/_deps) and reports unknown types. This script copies a
representative translation unit's command for every header so clangd resolves
the correct include paths and defines.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", required=True, help="Path to compile_commands.json")
    parser.add_argument(
        "--include",
        action="append",
        required=True,
        help="Header root to index (repeatable)",
    )
    parser.add_argument(
        "--representative",
        action="append",
        default=[],
        help="Substring of the source file whose command should be copied (repeatable)",
    )
    args = parser.parse_args()

    db_path = Path(args.db)
    if not db_path.is_file():
        print(f"[clangd] no compilation database at {db_path}; skipping")
        return 0

    try:
        entries = json.loads(db_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[clangd] failed to read {db_path}: {exc}", file=sys.stderr)
        return 0

    if not entries:
        print(f"[clangd] {db_path} is empty; skipping", file=sys.stderr)
        return 0

    representative = None
    for hint in args.representative:
        representative = next(
            (entry for entry in entries if hint in entry.get("file", "")), None
        )
        if representative is not None:
            break
    if representative is None:
        representative = next(
            (entry for entry in entries if "/src/" in entry.get("file", "")),
            entries[0],
        )

    existing = {entry.get("file") for entry in entries}
    added = 0
    for include_root in args.include:
        root = Path(include_root).resolve()
        if not root.is_dir():
            continue
        headers = list(root.rglob("*.hpp")) + list(root.rglob("*.h"))
        for header in sorted(headers):
            header_path = str(header.resolve())
            if header_path in existing:
                continue
            command = representative["command"].replace(
                representative["file"], header_path
            )
            entries.append(
                {
                    "directory": representative["directory"],
                    "command": command,
                    "file": header_path,
                }
            )
            existing.add(header_path)
            added += 1

    if added:
        db_path.write_text(json.dumps(entries, indent=2) + "\n")
    print(f"[clangd] {db_path}: added {added} header entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
