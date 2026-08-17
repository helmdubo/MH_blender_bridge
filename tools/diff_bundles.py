"""Reference bundle differ CLI (stage-B acceptance, executable Analyzer spec).

    python3 tools/diff_bundles.py <old_bundle_dir> <new_bundle_dir> \
        [--old-rel PATH] [--new-rel PATH]

Loads export_manifest.json and every listed .composite from both bundle
directories (trusting only the manifests, §1.1) and prints the
mh.diff_report JSON to stdout. --old-rel/--new-rel are the bundle
directories' paths relative to source_root (D27) for MOVE detection.
"""

import argparse
import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(TOOLS_DIR), "addon"))

from mh4blend.core.diff import diff_bundles  # noqa: E402


def load_bundle(bundle_dir):
    pending_path = os.path.join(bundle_dir, "export_manifest.json.tmp")
    if os.path.exists(pending_path):
        raise SystemExit(
            f"export in progress or recovery required in {bundle_dir}: "
            "export_manifest.json.tmp exists")
    manifest_path = os.path.join(bundle_dir, "export_manifest.json")
    if not os.path.exists(manifest_path):
        raise SystemExit(f"no export_manifest.json in {bundle_dir} — not a bundle")
    with open(manifest_path, "rb") as f:
        manifest_bytes = f.read()
    manifest = json.loads(manifest_bytes.decode("utf-8"))
    composites = {}
    for entry in manifest.get("resources", []):
        if entry.get("kind") == "composite":
            with open(os.path.join(bundle_dir, entry["source"]),
                      encoding="utf-8") as f:
                composites[entry["uid"]] = json.load(f)
    # Close the race with a concurrent exporter: either its marker is still
    # present, or the stable manifest bytes changed when it committed.
    if os.path.exists(pending_path):
        raise SystemExit(
            f"export started while reading {bundle_dir}: "
            "export_manifest.json.tmp exists")
    with open(manifest_path, "rb") as f:
        if f.read() != manifest_bytes:
            raise SystemExit(
                f"export_manifest.json changed while reading {bundle_dir}")
    return manifest, composites


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("old_bundle")
    parser.add_argument("new_bundle")
    parser.add_argument("--old-rel", default="")
    parser.add_argument("--new-rel", default="")
    args = parser.parse_args()

    old_manifest, old_composites = load_bundle(args.old_bundle)
    new_manifest, new_composites = load_bundle(args.new_bundle)
    report = diff_bundles(old_manifest, new_manifest,
                          old_composites, new_composites,
                          args.old_rel, args.new_rel)
    json.dump(report, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
