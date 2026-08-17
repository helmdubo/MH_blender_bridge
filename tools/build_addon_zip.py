"""Build the installable addon zip: dist/mh4blend-<version>.zip.

Blender's 'Install from Disk' expects a zip whose ROOT contains the addon
package directory (mh4blend/__init__.py at 'mh4blend/__init__.py' inside
the archive) — pointing it at a repo folder or zipping a parent directory
produces "No module named 'mh4blend'".

    python3 tools/build_addon_zip.py

Pure stdlib, no bpy.
"""

import ast
import os
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)
ADDON_DIR = os.path.join(REPO_ROOT, "addon", "mh4blend")
DIST_DIR = os.path.join(REPO_ROOT, "dist")

EXCLUDE_DIRS = {"__pycache__"}
EXCLUDE_SUFFIXES = (".pyc",)


def addon_version():
    with open(os.path.join(ADDON_DIR, "__init__.py"), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and node.targets[0].id == "bl_info":
            version = ast.literal_eval(node.value)["version"]
            return ".".join(str(part) for part in version)
    raise SystemExit("bl_info not found in addon/mh4blend/__init__.py")


def main():
    os.makedirs(DIST_DIR, exist_ok=True)
    zip_path = os.path.join(DIST_DIR, f"mh4blend-{addon_version()}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for root, dirs, files in os.walk(ADDON_DIR):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for name in sorted(files):
                if name.endswith(EXCLUDE_SUFFIXES):
                    continue
                full = os.path.join(root, name)
                # zip root = mh4blend/... — exactly what Blender expects
                arcname = os.path.relpath(full, os.path.dirname(ADDON_DIR))
                archive.write(full, arcname)
    names = zipfile.ZipFile(zip_path).namelist()
    assert "mh4blend/__init__.py" in names, "zip layout broken"
    assert "mh4blend/scene/export_bundle.py" not in names, \
        "retired Bundle Export API must not ship"
    print(f"built: {zip_path}  ({len(names)} files)")
    print("install: Blender > Edit > Preferences > Add-ons > "
          "(v) Install from Disk... > select this zip")


if __name__ == "__main__":
    main()
