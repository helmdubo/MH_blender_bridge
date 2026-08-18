"""Build and validate the self-contained Blender Extension package.

The extension bundles the pinned native ``xxhash`` wheel declared by
``blender_manifest.toml``. Blender's extension installer extracts the matching
wheel into extension-local site-packages, so artists never run pip manually.

Usage::

    python tools/build_addon_zip.py

Set ``MH_BLENDER`` when Blender is not on PATH or installed in the default
Windows 4.5 location.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib
import zipfile


TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
ADDON_DIR = REPO_ROOT / "addon" / "mh4blend"
DIST_DIR = REPO_ROOT / "dist"
MANIFEST_PATH = ADDON_DIR / "blender_manifest.toml"
DEFAULT_WINDOWS_BLENDER = Path(
    r"C:\Program Files\Blender Foundation\Blender 4.5\blender.exe")


def _blender_executable() -> str:
    configured = os.environ.get("MH_BLENDER", "").strip()
    if configured:
        candidate = Path(configured)
        if candidate.is_file():
            return str(candidate)
        raise SystemExit(f"MH_BLENDER does not exist: {candidate}")
    discovered = shutil.which("blender")
    if discovered:
        return discovered
    if DEFAULT_WINDOWS_BLENDER.is_file():
        return str(DEFAULT_WINDOWS_BLENDER)
    raise SystemExit(
        "Blender not found. Set MH_BLENDER to the Blender executable.")


def _run(command: list[str]) -> None:
    completed = subprocess.run(command, check=False)
    if completed.returncode:
        raise SystemExit(completed.returncode)


def main() -> None:
    manifest = tomllib.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    version = manifest["version"]
    platforms = manifest.get("platforms", [])
    platform_suffix = platforms[0] if len(platforms) == 1 else "multi-platform"
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    zip_path = DIST_DIR / f"mh4blend-{version}-{platform_suffix}.zip"
    if zip_path.exists():
        zip_path.unlink()

    blender = _blender_executable()
    _run([
        blender,
        "--command", "extension", "build",
        "--source-dir", str(ADDON_DIR),
        "--output-filepath", str(zip_path),
    ])
    _run([
        blender,
        "--command", "extension", "validate", str(zip_path),
    ])

    with zipfile.ZipFile(zip_path) as archive:
        names = set(archive.namelist())
    required = {
        "__init__.py",
        "blender_manifest.toml",
        "wheels/xxhash-4.0.1-cp311-cp311-win_amd64.whl",
    }
    missing = required - names
    if missing:
        raise SystemExit(
            "extension package is missing: " + ", ".join(sorted(missing)))
    retired_runtime_paths = {
        "scene/export_bundle.py",
        "scene/source_manifest.py",
        "core/source_resolver.py",
    }
    shipped_retired = retired_runtime_paths & names
    if shipped_retired:
        raise SystemExit(
            "retired manifest/bundle runtime must not ship: "
            + ", ".join(sorted(shipped_retired)))

    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    print(f"built: {zip_path}")
    print(f"sha256: {digest}")
    print("install: Blender > Edit > Preferences > Get Extensions > "
          "Install from Disk")


if __name__ == "__main__":
    main()
