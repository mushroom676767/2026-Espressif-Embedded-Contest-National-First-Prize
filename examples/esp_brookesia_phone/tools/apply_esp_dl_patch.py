#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 liuchang925
# SPDX-License-Identifier: Apache-2.0
"""Create a reproducible patched local esp-dl override without editing managed dependencies."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def normalized_hash(path: Path) -> str:
    return hashlib.sha256(path.read_text(encoding="utf-8").encode("utf-8")).hexdigest()


def verify_version(component: Path, expected: str) -> None:
    manifest = (component / "idf_component.yml").read_text(encoding="utf-8")
    match = re.search(r"^version:\s*['\"]?([^'\"\s]+)", manifest, re.MULTILINE)
    if match is None or match.group(1) != expected:
        raise ValueError(f"Expected esp-dl {expected}; incompatible component: {component}")


def apply(project: Path, check_only: bool) -> None:
    patch_dir = project / "patches"
    metadata = json.loads((patch_dir / "esp-dl-3.3.7-recognition-id.json").read_text(encoding="utf-8"))
    patch = patch_dir / "esp-dl-3.3.7-recognition-id.patch"
    if hashlib.sha256(patch.read_bytes()).hexdigest() != metadata["patch_sha256"]:
        raise ValueError("Patch checksum mismatch")

    components = project / "components"
    target = components / "espressif__esp-dl"
    if target.exists():
        verify_version(target, metadata["version"])
        if normalized_hash(target / metadata["file"]) != metadata["patched_sha256_lf"]:
            raise ValueError("Existing local esp-dl differs from this patch; preserving it without changes")
        print("esp-dl 3.3.7 recognition ID patch already applied (local override)")
        return

    source = project / "managed_components/espressif__esp-dl"
    if not source.is_dir():
        raise ValueError("Dependencies missing. Run idf.py reconfigure in the project directory first")
    verify_version(source, metadata["version"])
    if normalized_hash(source / metadata["file"]) != metadata["upstream_sha256_lf"]:
        raise ValueError("Managed esp-dl source is not the expected pristine version; refusing to overwrite changes")
    if check_only:
        print("esp-dl 3.3.7 source and patch checksums OK; ready to create local override")
        return

    git_root = Path(subprocess.check_output(
        ["git", "-C", str(project), "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    components.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".esp-dl-patch-", dir=components))
    try:
        shutil.copytree(source, staging, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns(".git", ".component_hash", "CHECKSUMS.json"))
        relative = staging.relative_to(git_root).as_posix()
        command = ["git", "-C", str(git_root), "apply", f"--directory={relative}"]
        subprocess.run(command + ["--check", str(patch)], check=True)
        subprocess.run(command + [str(patch)], check=True)
        if normalized_hash(staging / metadata["file"]) != metadata["patched_sha256_lf"]:
            raise ValueError("Patched file checksum mismatch")
        staging.rename(target)
    finally:
        if staging.exists():
            resolved = staging.resolve()
            if resolved.parent != components.resolve() or not resolved.name.startswith(".esp-dl-patch-"):
                raise ValueError("Refusing to remove staging directory outside project components")
            shutil.rmtree(resolved)
    print(f"Patched local esp-dl created at {target}")
    print("Managed dependencies were preserved. Run idf.py reconfigure before building.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Validate compatibility without writing files")
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        apply(args.project_root.resolve(), args.check)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"esp-dl patch failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
