#!/usr/bin/env python3
"""Fail unless PE's local shared components match the reviewed content lock."""

import hashlib
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
LOCK_PATH = HERE / "shared_components.lock.json"


def component_digest(root: Path, name: str) -> str:
    digest = hashlib.sha256()
    component = root / name
    if not component.is_dir():
        raise SystemExit(f"missing shared component: {name}")
    for path in sorted(component.rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            relative = path.relative_to(root).as_posix().encode()
            digest.update(relative + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def main() -> None:
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    root = (HERE / lock["source_path"]).resolve()
    mismatches = [
        name
        for name, expected in lock["components"].items()
        if component_digest(root, name) != expected
    ]
    if mismatches:
        raise SystemExit("shared component lock mismatch: " + ", ".join(mismatches))
    print("shared component lock verified")


if __name__ == "__main__":
    main()
