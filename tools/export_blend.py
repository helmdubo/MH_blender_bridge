"""Headless bundle export of one .blend (B10 CLI).

    python3 tools/export_blend.py <file.blend> <bundle_dir>
    blender -b <file.blend> -P tools/export_blend.py -- <bundle_dir>

Prints the export report (validation, written/skipped/deleted) as JSON.
Exit code 1 when validation blocked the export.
"""

import json
import os
import sys

import bpy

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(TOOLS_DIR), "addon"))

from mh4blend.scene.export_bundle import export_bundle  # noqa: E402


def main():
    argv = sys.argv
    if "--" in argv:  # blender -b file.blend -P script -- out_dir
        out_dir = argv[argv.index("--") + 1]
    else:
        blend, out_dir = argv[1], argv[2]
        bpy.ops.wm.open_mainfile(filepath=os.path.abspath(blend))
    report = export_bundle(os.path.abspath(out_dir))
    json.dump(report, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")
    sys.exit(0 if report["ok"] else 1)


if __name__ == "__main__":
    main()
