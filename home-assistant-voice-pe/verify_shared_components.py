#!/usr/bin/env python3
"""Fail unless PE's shared components match the reviewed content lock.

Two groups: `components` are consumed straight from the Satellite1 checkout; and
`identical_local_components` are PE-local copies that must be byte-identical to Satellite1's.
"""

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

    # Components PE builds from its OWN copy (external_components path esphome/components) but that
    # must stay byte-identical to the Satellite1 source. Per-device differences belong in YAML
    # options, not in divergent text. Checked three ways: PE copy == lock, source == lock, and
    # therefore PE copy == source.
    local_root = HERE / "esphome" / "components"
    drift = []
    for name, expected in lock.get("identical_local_components", {}).items():
        if component_digest(local_root, name) != expected:
            drift.append(f"{name} (PE copy differs from lock)")
        if component_digest(root, name) != expected:
            drift.append(f"{name} (satellite1 source differs from lock)")
    if drift:
        raise SystemExit(
            "shared component drift: " + ", ".join(drift)
            + ". Make the two copies identical, then update the lock hash."
        )
    print("shared component lock verified")


if __name__ == "__main__":
    main()
